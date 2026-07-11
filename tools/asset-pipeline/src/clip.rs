// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The cooker's in-memory animation clip and its RMA1 clip-payload encoder, plus the glTF animation
//! import (M6.7, AN0). A clip stores per-joint keyframed translation / rotation / scale tracks; the
//! CPU sampler in `engine/assets` evaluates it at a time t into a skinning palette. The cooked layout
//! is **sparse and columnar**: a channel table (one record per non-silent track) followed by a
//! keyframe blob (each channel's times then values, in table order) — shaped for the sampler, not for
//! glTF's per-accessor form. The encoder writes exactly what `decode_clip` validates, and both sides
//! embed the same schema hash.
//!
//! Import lifts each glTF animation into a clip, resolving every channel's target node to the
//! skeleton joint index the skin import assigned (dropping channels that target non-skeleton nodes).
//! Only STEP and LINEAR interpolation are cooked; CUBICSPLINE is rejected with a clear message (a
//! quality seam a later brick can take).

use std::collections::HashMap;

use crate::cooked::{wrap_container, ByteWriter, ASSET_KIND_CLIP, CLIP_SCHEMA_HASH};
use crate::PipelineError;

/// Which TRS path a channel drives. Wire values match the engine's channel-record `path` field.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChannelPath {
    Translation = 0,
    Rotation = 1,
    Scale = 2,
}

/// How a track holds its value between keyframes. Wire values match the engine's `Interpolation`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Interp {
    Step = 0,
    Linear = 1,
}

/// One keyframed track: a target joint, a path, an interpolation mode, strictly-increasing `times`,
/// and a flat `values` array of `times.len() * components` floats (3 for translation/scale, 4 for a
/// rotation quaternion) in keyframe order.
#[derive(Debug, Clone)]
pub struct Channel {
    pub target_joint: u32,
    pub path: ChannelPath,
    pub interp: Interp,
    pub times: Vec<f32>,
    pub values: Vec<f32>,
}

/// A cook-ready clip: a name (for the output filename), a duration in seconds, the joint count of the
/// skeleton it animates (so the engine can size its dense per-joint table), and the sparse channels.
#[derive(Debug, Clone)]
pub struct Clip {
    pub name: String,
    pub duration: f32,
    pub joint_count: u32,
    pub channels: Vec<Channel>,
}

impl Clip {
    /// Encode this clip into a complete RMA1 file, returning `(bytes, asset_id)`. The layout mirrors
    /// `decode_clip` in the engine's reader exactly.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let mut p = ByteWriter::new();
        p.f32(self.duration);
        p.u32(self.joint_count);
        p.u32(self.channels.len() as u32);
        // The channel table, then the keyframe blob (times then values, per channel, in table order).
        for c in &self.channels {
            p.u32(c.target_joint);
            p.u32(c.path as u32);
            p.u32(c.interp as u32);
            p.u32(c.times.len() as u32);
        }
        for c in &self.channels {
            for &t in &c.times {
                p.f32(t);
            }
            for &v in &c.values {
                p.f32(v);
            }
        }
        wrap_container(ASSET_KIND_CLIP, CLIP_SCHEMA_HASH, &p.into_vec())
    }
}

/// Import every animation in the document into a clip, resolving channel targets to joint indices via
/// `node_to_joint` (the map the skin import produced). `joint_count` is the skeleton's joint count,
/// stamped into each clip so the engine sizes its dense track table. A channel targeting a node that
/// is not a skeleton joint is dropped (it animates something a skin does not deform); an animation
/// left with no skeleton channels produces no clip.
pub fn import_clips(
    document: &gltf::Document,
    buffers: &[gltf::buffer::Data],
    joint_count: u32,
    node_to_joint: &HashMap<usize, u32>,
) -> Result<Vec<Clip>, PipelineError> {
    let mut clips = Vec::new();
    for (i, animation) in document.animations().enumerate() {
        let name = animation
            .name()
            .map(|n| n.to_string())
            .unwrap_or_else(|| format!("anim{i}"));

        let mut channels = Vec::new();
        let mut duration = 0.0f32;
        for channel in animation.channels() {
            let target_node = channel.target().node().index();
            let target_joint = match node_to_joint.get(&target_node) {
                Some(&j) => j,
                None => continue, // animates a non-skeleton node — not this skin's concern
            };

            use gltf::animation::Property;
            let path = match channel.target().property() {
                Property::Translation => ChannelPath::Translation,
                Property::Rotation => ChannelPath::Rotation,
                Property::Scale => ChannelPath::Scale,
                Property::MorphTargetWeights => {
                    eprintln!("warning: morph-target animation is not cooked in v1; skipping");
                    continue;
                }
            };

            use gltf::animation::Interpolation as GltfInterp;
            let interp = match channel.sampler().interpolation() {
                GltfInterp::Linear => Interp::Linear,
                GltfInterp::Step => Interp::Step,
                GltfInterp::CubicSpline => {
                    return Err(PipelineError::Unsupported(
                        "CUBICSPLINE animation is not cooked in v1 (re-export as LINEAR/STEP)"
                            .to_string(),
                    ));
                }
            };

            let reader = channel.reader(|b| buffers.get(b.index()).map(|d| d.0.as_slice()));
            let times: Vec<f32> = reader
                .read_inputs()
                .ok_or_else(|| {
                    PipelineError::Unsupported("animation channel has no input times".to_string())
                })?
                .collect();

            use gltf::animation::util::ReadOutputs;
            let values: Vec<f32> = match reader.read_outputs().ok_or_else(|| {
                PipelineError::Unsupported("animation channel has no output values".to_string())
            })? {
                ReadOutputs::Translations(it) => it.flatten().collect(),
                ReadOutputs::Scales(it) => it.flatten().collect(),
                ReadOutputs::Rotations(rot) => rot.into_f32().flatten().collect(),
                ReadOutputs::MorphTargetWeights(_) => continue, // guarded by the path match above
            };

            if let Some(&last) = times.last() {
                duration = duration.max(last);
            }
            channels.push(Channel {
                target_joint,
                path,
                interp,
                times,
                values,
            });
        }

        if channels.is_empty() {
            continue; // an animation that touches no skeleton joint is not a skinning clip
        }
        clips.push(Clip {
            name,
            duration,
            joint_count,
            channels,
        });
    }
    Ok(clips)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cooked::{read_header, ASSET_KIND_CLIP, CLIP_SCHEMA_HASH};

    fn one_channel_clip() -> Clip {
        Clip {
            name: "walk".to_string(),
            duration: 1.0,
            joint_count: 2,
            channels: vec![Channel {
                target_joint: 0,
                path: ChannelPath::Translation,
                interp: Interp::Linear,
                times: vec![0.0, 1.0],
                values: vec![0.0, 0.0, 0.0, 6.0, 0.0, 0.0],
            }],
        }
    }

    #[test]
    fn cook_is_byte_stable_and_carries_the_clip_kind_and_schema() {
        let clip = one_channel_clip();
        let (a, id_a) = clip.cook();
        let (b, id_b) = clip.cook();
        assert_eq!(a, b, "cook must be deterministic");
        assert_eq!(id_a, id_b);

        let (header, payload) = read_header(&a).unwrap();
        assert_eq!(header.asset_kind, ASSET_KIND_CLIP);
        assert_eq!(header.type_schema_hash, CLIP_SCHEMA_HASH);
        // duration(4) + joint_count(4) + channel_count(4) + 1 record(16) + blob(2 times + 6 values).
        assert_eq!(payload.len(), 12 + 16 + (2 + 6) * 4);
    }

    #[test]
    fn schema_hash_is_the_constant_the_engine_pins() {
        assert_eq!(CLIP_SCHEMA_HASH, 0x6C84_D2A2_AAAB_CE49);
    }

    #[test]
    fn wire_enum_values_match_the_engine() {
        assert_eq!(ChannelPath::Translation as u32, 0);
        assert_eq!(ChannelPath::Rotation as u32, 1);
        assert_eq!(ChannelPath::Scale as u32, 2);
        assert_eq!(Interp::Step as u32, 0);
        assert_eq!(Interp::Linear as u32, 1);
    }
}
