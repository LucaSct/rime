// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The cooker's in-memory skeleton and its RMA1 skeleton-payload encoder, plus the glTF skin import
//! (M6.7, AN0). A skeleton is the joint hierarchy and bind pose a skinned mesh deforms against; the
//! CPU sampler in `engine/assets` walks it (see `docs/math/skinning.md`). The encoder writes exactly
//! the byte layout the C++ `decode_skeleton` validates, and both sides embed the same schema hash, so
//! they agree on the format by construction.
//!
//! Import lifts glTF's skin — a flat joint list, per-joint inverse-bind matrices, and each joint
//! node's rest transform — into that form, with one correctness step glTF does not guarantee:
//! **topological reordering**. The engine stores joints parent-before-child so the sampler composes
//! world poses in a single forward pass; glTF's `skin.joints` array may be in any order, so we sort
//! it by tree depth and hand back a remap the mesh (JOINTS_0) and clips (channel targets) apply to
//! their joint references.

use std::collections::HashMap;

use crate::cooked::{
    fnv1a_64, wrap_container, ByteWriter, ASSET_KIND_SKELETON, SKELETON_SCHEMA_HASH,
};
use crate::math;
use crate::PipelineError;

/// One joint (bone) in cooked form. Fields are in wire order and mirror the engine's `Joint`:
/// `parent` is an index into `Skeleton::joints` (or -1 for a root), `name_hash` is the FNV-1a of the
/// joint's name (lookup without storing strings), `inverse_bind` is the column-major model→bind-local
/// matrix, and `translation`/`rotation`/`scale` are the bind-pose local transform the sampler uses
/// where a clip is silent.
#[derive(Debug, Clone)]
pub struct Joint {
    pub parent: i32,
    pub name_hash: u64,
    pub inverse_bind: [f32; 16],
    pub translation: [f32; 3],
    pub rotation: [f32; 4],
    pub scale: [f32; 3],
}

/// A cooked-ready skeleton: its joints in **topological order** (every parent precedes its children).
#[derive(Debug, Default, Clone)]
pub struct Skeleton {
    pub joints: Vec<Joint>,
}

impl Skeleton {
    /// Encode this skeleton into a complete RMA1 file, returning `(bytes, asset_id)`. The layout
    /// mirrors `decode_skeleton` in the engine's reader exactly: a joint count, then each joint's
    /// fixed record.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let mut p = ByteWriter::new();
        p.u32(self.joints.len() as u32);
        for j in &self.joints {
            p.i32(j.parent);
            p.u64(j.name_hash);
            for m in j.inverse_bind {
                p.f32(m);
            }
            for t in j.translation {
                p.f32(t);
            }
            for r in j.rotation {
                p.f32(r);
            }
            for s in j.scale {
                p.f32(s);
            }
        }
        wrap_container(ASSET_KIND_SKELETON, SKELETON_SCHEMA_HASH, &p.into_vec())
    }
}

/// A skeleton imported from glTF, plus the two remap tables its dependents need. Because import
/// reorders joints into topological order, a JOINTS_0 index (into glTF's original `skin.joints`
/// order) and an animation-channel target (a glTF node index) must both be translated to the new
/// joint order — `joint_remap[skin_index]` and `node_to_joint[node_index]` do exactly that.
pub struct ImportedSkeleton {
    pub skeleton: Skeleton,
    pub joint_remap: Vec<u32>, // glTF skin-joint order → topological joint index
    pub node_to_joint: HashMap<usize, u32>, // glTF node index → topological joint index
}

/// Import the document's first skin as a skeleton, or `Ok(None)` if the file has no skin. Additional
/// skins are ignored with a warning (v1 cooks one skeleton per model). The joints are topologically
/// reordered; see the module docs for why and what the returned remaps are for.
pub fn import_skeleton(
    document: &gltf::Document,
    buffers: &[gltf::buffer::Data],
) -> Result<Option<ImportedSkeleton>, PipelineError> {
    let skin = match document.skins().next() {
        Some(s) => s,
        None => return Ok(None),
    };
    if document.skins().count() > 1 {
        eprintln!(
            "warning: glTF has {} skins; v1 cooks only the first as the skeleton",
            document.skins().count()
        );
    }

    // The skin's joint nodes, in glTF order — this is the order JOINTS_0 indices reference.
    let skin_joints: Vec<gltf::Node> = skin.joints().collect();
    let mut skin_index_of: HashMap<usize, usize> = HashMap::new();
    for (i, node) in skin_joints.iter().enumerate() {
        skin_index_of.insert(node.index(), i);
    }

    // glTF nodes hold children, not parents, so build the child→parent map once, then read off each
    // joint's parent *within the skin* (a joint whose parent is not itself a joint is a skin root).
    let mut parent_node: HashMap<usize, usize> = HashMap::new();
    for node in document.nodes() {
        for child in node.children() {
            parent_node.insert(child.index(), node.index());
        }
    }
    let parent_skin: Vec<Option<usize>> = skin_joints
        .iter()
        .map(|n| {
            parent_node
                .get(&n.index())
                .and_then(|p| skin_index_of.get(p).copied())
        })
        .collect();

    // Inverse-bind matrices: one per joint, in skin order. Absent means identity (glTF default).
    let reader = skin.reader(|b| buffers.get(b.index()).map(|d| d.0.as_slice()));
    let ibms: Vec<[[f32; 4]; 4]> = match reader.read_inverse_bind_matrices() {
        Some(it) => it.collect(),
        None => vec![
            [
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ];
            skin_joints.len()
        ],
    };
    if ibms.len() != skin_joints.len() {
        return Err(PipelineError::Unsupported(format!(
            "skin has {} joints but {} inverse-bind matrices",
            skin_joints.len(),
            ibms.len()
        )));
    }

    // Topological order: sort joints by their depth in the tree (a joint's number of ancestors within
    // the skin). Parents have strictly smaller depth than their children, so depth-order is a valid
    // topological order; a stable sort keeps siblings in glTF order for determinism.
    let depth = |mut i: usize| -> usize {
        let mut d = 0;
        while let Some(p) = parent_skin[i] {
            d += 1;
            i = p;
            if d > skin_joints.len() {
                break; // guard against a malformed cyclic parent chain
            }
        }
        d
    };
    let mut order: Vec<usize> = (0..skin_joints.len()).collect();
    order.sort_by_key(|&i| depth(i));

    // The inverse of `order`: where each glTF skin joint landed in the topological list.
    let mut joint_remap = vec![0u32; skin_joints.len()];
    for (new_idx, &old_idx) in order.iter().enumerate() {
        joint_remap[old_idx] = new_idx as u32;
    }

    let mut joints = Vec::with_capacity(order.len());
    let mut node_to_joint = HashMap::new();
    for &old in &order {
        let node = &skin_joints[old];
        let (translation, rotation, scale) = node.transform().decomposed();
        let parent = parent_skin[old]
            .map(|p| joint_remap[p] as i32)
            .unwrap_or(-1);
        joints.push(Joint {
            parent,
            name_hash: fnv1a_64(node.name().unwrap_or("").as_bytes()),
            inverse_bind: math::from_columns(ibms[old]),
            translation,
            rotation,
            scale,
        });
        node_to_joint.insert(node.index(), joint_remap[old]);
    }

    Ok(Some(ImportedSkeleton {
        skeleton: Skeleton { joints },
        joint_remap,
        node_to_joint,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cooked::{read_header, ASSET_KIND_SKELETON, SKELETON_SCHEMA_HASH};

    fn two_joint_skeleton() -> Skeleton {
        Skeleton {
            joints: vec![
                Joint {
                    parent: -1,
                    name_hash: 0xA,
                    inverse_bind: math::IDENTITY,
                    translation: [0.0, 0.0, 0.0],
                    rotation: [0.0, 0.0, 0.0, 1.0],
                    scale: [1.0, 1.0, 1.0],
                },
                Joint {
                    parent: 0,
                    name_hash: 0xB,
                    inverse_bind: {
                        let mut m = math::IDENTITY;
                        m[12] = -2.0;
                        m
                    },
                    translation: [2.0, 0.0, 0.0],
                    rotation: [0.0, 0.0, 0.0, 1.0],
                    scale: [1.0, 1.0, 1.0],
                },
            ],
        }
    }

    #[test]
    fn cook_is_byte_stable_and_carries_the_skeleton_kind_and_schema() {
        let sk = two_joint_skeleton();
        let (a, id_a) = sk.cook();
        let (b, id_b) = sk.cook();
        assert_eq!(a, b, "cook must be deterministic");
        assert_eq!(id_a, id_b);

        let (header, payload) = read_header(&a).unwrap();
        assert_eq!(header.asset_kind, ASSET_KIND_SKELETON);
        assert_eq!(header.type_schema_hash, SKELETON_SCHEMA_HASH);
        // joint_count(4) + 2 × 116-byte joint records.
        assert_eq!(payload.len(), 4 + 2 * 116);
    }

    #[test]
    fn schema_hash_is_the_constant_the_engine_pins() {
        assert_eq!(SKELETON_SCHEMA_HASH, 0xD90A_5CB8_EBA3_6DED);
    }
}
