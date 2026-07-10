// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The cooker's in-memory mesh and its RMA1 mesh-payload encoder. The base vertex layout is the v1
//! position/normal/uv interleave (32-byte stride) that `engine/assets` and `engine/render` consume;
//! a normal-mapped mesh (M6.4) additionally carries a per-vertex tangent, which widens the stride to
//! 48 bytes and sets one more attribute flag — an *additive* growth (ADR-0024 decision 6), not a new
//! container version. The encoder writes exactly the byte layout the C++ reader validates (see
//! `docs/design/assets.md`), and the reader re-derives the stride from the flags, so neither side can
//! disagree on where a vertex ends.

use crate::cooked::{wrap_container, ByteWriter, ASSET_KIND_MESH, MESH_SCHEMA_HASH};
use crate::math;

/// Vertex attribute flags (match `engine/assets/mesh_asset.hpp`). A v1 mesh always cooks
/// position|normal|uv; a normal-mapped mesh (M6.4) adds `tangent`; a skinned mesh (M6.7) adds
/// `joints` + `weights`. Each is an additive flag and a wider stride, never a new container version.
pub const ATTR_POSITION: u32 = 1 << 0;
pub const ATTR_NORMAL: u32 = 1 << 1;
pub const ATTR_UV: u32 = 1 << 2;
pub const ATTR_TANGENT: u32 = 1 << 3;
pub const ATTR_JOINTS: u32 = 1 << 4;
pub const ATTR_WEIGHTS: u32 = 1 << 5;

/// Interleaved vertex stride without / with tangents (bytes): pos+normal+uv is 32, +tangent is 48.
/// These mirror `expected_vertex_stride` in the engine's `mesh_asset.hpp`; the reader validates the
/// declared stride against the attribute flags, so a corrupt stride can't walk vertex addressing off
/// the blob.
pub const STRIDE_NO_TANGENT: u32 = 32;
pub const STRIDE_WITH_TANGENT: u32 = 48;
/// Extra bytes a per-vertex tangent (4×f32) and a skin binding (4×u16 joints + 4×f32 weights) add to
/// the stride. Attributes are laid out in flag-bit order — position, normal, uv, tangent, joints,
/// weights — so a skinned+tangented vertex is 32 + 16 + 24 = 72 bytes; skinned-only is 56.
pub const TANGENT_BYTES: u32 = 4 * 4;
pub const SKIN_BYTES: u32 = 4 * 2 + 4 * 4;

/// The v1 vertex: position, normal, uv — 32 bytes interleaved, matching the engine's `MeshVertex`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Vertex {
    pub position: [f32; 3],
    pub normal: [f32; 3],
    pub uv: [f32; 2],
}

/// One drawable range of the index buffer, tagged with a material slot (0 until materials, M6.4).
#[derive(Debug, Clone, Copy)]
pub struct Submesh {
    pub first_index: u32,
    pub index_count: u32,
    pub material_slot: u32,
}

/// A mesh's per-vertex skin binding (M6.7): for each vertex, the (up to four) skeleton joints that
/// deform it and the weight of each. `joints` indexes into the `SkeletonAsset` cooked alongside;
/// `weights` are normalized to sum to 1. glTF supplies these as JOINTS_0 / WEIGHTS_0; influences
/// beyond four (a second JOINTS_1 set) are dropped at import with a warning, and the surviving four
/// renormalized — the common, cheap case AN0 targets. It rides beside `Vertex` (not inside it) for
/// the same reason `tangents` does: it is optional and its presence is what flips the ATTR_JOINTS /
/// ATTR_WEIGHTS flags and widens the stride.
#[derive(Debug, Default, Clone)]
pub struct SkinWeights {
    pub joints: Vec<[u16; 4]>,
    pub weights: Vec<[f32; 4]>,
}

/// Renormalize a vertex's four skin weights so they sum to 1 — the invariant a linear-blend skin
/// assumes (Σ wᵢ = 1 keeps a rigid pose rigid; see docs/math/skinning.md). glTF authoring tools may
/// emit weights that sum to slightly off 1 (quantization) or, after dropping a >4th influence, less
/// than 1. A vertex with no influence at all (all zero) is pinned fully to its first joint rather
/// than left weightless, so it follows the skeleton instead of collapsing to the origin.
pub fn normalize_weights(w: [f32; 4]) -> [f32; 4] {
    let sum = w[0] + w[1] + w[2] + w[3];
    if sum > 0.0 {
        [w[0] / sum, w[1] / sum, w[2] / sum, w[3] / sum]
    } else {
        [1.0, 0.0, 0.0, 0.0]
    }
}

/// A cooked-ready mesh: interleaved vertices, a 32-bit triangle-list index buffer, and a submesh
/// table (one entry per source primitive). `tangents`, when present, is a parallel array — one
/// `[x, y, z, handedness]` per vertex — generated at import (M6.4, see `tangent.rs`) only for meshes
/// a normal map needs. It lives beside `Vertex` rather than inside it because it is optional and
/// derived *after* primitives merge; its presence is what flips `ATTR_TANGENT` and the wider stride
/// in `cook`.
#[derive(Debug, Default, Clone)]
pub struct Mesh {
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u32>,
    pub submeshes: Vec<Submesh>,
    pub tangents: Option<Vec<[f32; 4]>>,
    /// Per-vertex skin binding, present iff the mesh is skinned (M6.7). Parallel to `vertices`.
    pub skin: Option<SkinWeights>,
}

impl Mesh {
    /// Merge several world-space primitives into one mesh: vertices are concatenated, each
    /// primitive's indices are rebased onto its vertex block, and each becomes one submesh. This is
    /// how a glTF file's primitives (already flattened to world space at import) become a single
    /// cooked mesh with a submesh table.
    pub fn from_primitives(primitives: Vec<Primitive>) -> Self {
        let mut mesh = Mesh::default();
        // The merged mesh is skinned iff any primitive is; a non-skinned primitive folded into a
        // skinned mesh has its vertices pinned fully to joint 0, so every vertex has a binding and the
        // interleaved blob stays rectangular (one stride for the whole mesh).
        if primitives.iter().any(|p| p.skin.is_some()) {
            mesh.skin = Some(SkinWeights::default());
        }
        for prim in primitives {
            let base = mesh.vertices.len() as u32;
            let first_index = mesh.indices.len() as u32;
            let vertex_count = prim.vertices.len();
            mesh.indices.extend(prim.indices.iter().map(|&i| i + base));
            mesh.submeshes.push(Submesh {
                first_index,
                index_count: prim.indices.len() as u32,
                material_slot: prim.material_slot,
            });
            mesh.vertices.extend(prim.vertices);
            if let Some(dst) = &mut mesh.skin {
                match prim.skin {
                    Some(s) => {
                        dst.joints.extend(s.joints);
                        dst.weights.extend(s.weights);
                    }
                    None => {
                        dst.joints
                            .extend(std::iter::repeat_n([0u16; 4], vertex_count));
                        dst.weights
                            .extend(std::iter::repeat_n([1.0f32, 0.0, 0.0, 0.0], vertex_count));
                    }
                }
            }
        }
        mesh
    }

    pub fn is_empty(&self) -> bool {
        self.vertices.is_empty() || self.indices.is_empty()
    }

    /// The local-space axis-aligned bounds over the (already world-flattened) vertex positions.
    pub fn aabb(&self) -> ([f32; 3], [f32; 3]) {
        let mut min = [f32::INFINITY; 3];
        let mut max = [f32::NEG_INFINITY; 3];
        for v in &self.vertices {
            for axis in 0..3 {
                min[axis] = min[axis].min(v.position[axis]);
                max[axis] = max[axis].max(v.position[axis]);
            }
        }
        (min, max)
    }

    /// Encode this mesh into a complete RMA1 file, returning `(bytes, asset_id)`. The layout mirrors
    /// `decode_mesh` in the engine's reader exactly.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        // Optional attributes are additive: each present one sets its flag bit(s) and widens the
        // stride, with no new container version. The declared stride and flags travel together, and
        // the C++ reader re-derives the stride from the flags — so meshes at any attribute level
        // differ only in these two header words plus the extra per-vertex bytes. Flag-bit order is the
        // blob order: position, normal, uv, tangent, then the skin's joints and weights.
        let mut attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_UV;
        let mut stride = STRIDE_NO_TANGENT;
        if self.tangents.is_some() {
            attribs |= ATTR_TANGENT;
            stride += TANGENT_BYTES;
        }
        if self.skin.is_some() {
            attribs |= ATTR_JOINTS | ATTR_WEIGHTS;
            stride += SKIN_BYTES;
        }
        // Every optional parallel array is one-per-vertex by construction. Assert it, so a hand-built
        // mesh can't cook a blob whose stride and vertex count silently disagree — the reader would
        // reject it, but failing here names why.
        debug_assert!(
            self.tangents
                .as_ref()
                .is_none_or(|t| t.len() == self.vertices.len()),
            "tangent array must have exactly one entry per vertex"
        );
        debug_assert!(
            self.skin
                .as_ref()
                .is_none_or(|s| s.joints.len() == self.vertices.len()
                    && s.weights.len() == self.vertices.len()),
            "skin joints and weights must each have exactly one entry per vertex"
        );
        let (min, max) = self.aabb();

        let mut p = ByteWriter::new();
        p.u32(attribs);
        p.u32(stride);
        p.u32(self.vertices.len() as u32);
        p.u32(self.indices.len() as u32);
        for c in min {
            p.f32(c);
        }
        for c in max {
            p.f32(c);
        }
        p.u32(self.submeshes.len() as u32);
        for s in &self.submeshes {
            p.u32(s.first_index);
            p.u32(s.index_count);
            p.u32(s.material_slot);
        }
        for (i, v) in self.vertices.iter().enumerate() {
            for c in v.position {
                p.f32(c);
            }
            for c in v.normal {
                p.f32(c);
            }
            for c in v.uv {
                p.f32(c);
            }
            // Tangent, then the skin (u16 joints, then f32 weights), matching the attribute order the
            // reader's `expected_vertex_stride` walks.
            if let Some(tangents) = &self.tangents {
                for c in tangents[i] {
                    p.f32(c);
                }
            }
            if let Some(skin) = &self.skin {
                for j in skin.joints[i] {
                    p.u16(j);
                }
                for w in skin.weights[i] {
                    p.f32(w);
                }
            }
        }
        for &i in &self.indices {
            p.u32(i);
        }
        wrap_container(ASSET_KIND_MESH, MESH_SCHEMA_HASH, &p.into_vec())
    }
}

/// One imported primitive in world space, before merging. `indices` are local to `vertices`;
/// `material_slot` is the index into the cooked model's material table this primitive shades with
/// (M6.4), resolved at import from the glTF primitive's material — a primitive with no material maps
/// to the appended default-material slot. It rides through `from_primitives` onto the submesh.
pub struct Primitive {
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u32>,
    pub material_slot: u32,
    /// Per-vertex skin binding, present iff this primitive came from a skinned node (M6.7). Parallel
    /// to `vertices`, with weights already renormalized at import.
    pub skin: Option<SkinWeights>,
}

/// Derive per-vertex geometric normals from the triangle list (used when a primitive has no NORMAL
/// attribute). Each triangle's face normal is accumulated onto its three vertices, then normalized.
/// For a mesh whose faces do not share vertices this is exact flat shading; where vertices are
/// shared it is the area-weighted average — good enough for v1, and the common case (models that
/// omit normals are usually planar or already split). Documented as a known simplification.
pub fn compute_normals(vertices: &[[f32; 3]], indices: &[u32]) -> Vec<[f32; 3]> {
    let mut normals = vec![[0.0f32; 3]; vertices.len()];
    for tri in indices.chunks_exact(3) {
        let (i0, i1, i2) = (tri[0] as usize, tri[1] as usize, tri[2] as usize);
        let p0 = vertices[i0];
        let p1 = vertices[i1];
        let p2 = vertices[i2];
        let edge1 = [p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]];
        let edge2 = [p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]];
        let face = math::cross(edge1, edge2); // length ∝ triangle area → area weighting
        for &i in &[i0, i1, i2] {
            for axis in 0..3 {
                normals[i][axis] += face[axis];
            }
        }
    }
    for n in &mut normals {
        *n = math::normalize(*n);
    }
    normals
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn computed_normals_of_a_planar_quad_face_plus_z() {
        let verts = [
            [0.0, 0.0, 0.0],
            [1.0, 0.0, 0.0],
            [1.0, 1.0, 0.0],
            [0.0, 1.0, 0.0],
        ];
        let idx = [0u32, 1, 2, 0, 2, 3];
        for n in compute_normals(&verts, &idx) {
            assert!((n[0]).abs() < 1e-6 && (n[1]).abs() < 1e-6 && (n[2] - 1.0).abs() < 1e-6);
        }
    }

    #[test]
    fn aabb_spans_the_vertices() {
        let mesh = Mesh {
            vertices: vec![
                Vertex {
                    position: [-1.0, 0.0, 2.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 0.0],
                },
                Vertex {
                    position: [3.0, -2.0, 5.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [1.0, 1.0],
                },
            ],
            ..Default::default()
        };
        let (min, max) = mesh.aabb();
        assert_eq!(min, [-1.0, -2.0, 2.0]);
        assert_eq!(max, [3.0, 0.0, 5.0]);
    }

    #[test]
    fn tangents_widen_the_stride_and_set_the_flag() {
        use crate::cooked::read_header;
        // One triangle, cooked twice: plain (P/N/UV) and with a tangent per vertex. The payload's
        // first two u32 words are the attribute flags and the stride; assert they change exactly as
        // the additive-attribute design promises, and that the blob grows by 16 bytes per vertex.
        let mut mesh = Mesh {
            vertices: vec![
                Vertex {
                    position: [0.0, 0.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 0.0],
                },
                Vertex {
                    position: [1.0, 0.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [1.0, 0.0],
                },
                Vertex {
                    position: [0.0, 1.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 1.0],
                },
            ],
            indices: vec![0, 1, 2],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 3,
                material_slot: 0,
            }],
            tangents: None,
            skin: None,
        };
        let (plain, _) = mesh.cook();
        let (_, plain_payload) = read_header(&plain).unwrap();
        assert_eq!(
            u32_at(plain_payload, 0),
            ATTR_POSITION | ATTR_NORMAL | ATTR_UV
        );
        assert_eq!(u32_at(plain_payload, 4), STRIDE_NO_TANGENT);

        mesh.tangents = Some(vec![[1.0, 0.0, 0.0, 1.0]; 3]);
        let (tan, _) = mesh.cook();
        let (_, tan_payload) = read_header(&tan).unwrap();
        assert_eq!(
            u32_at(tan_payload, 0),
            ATTR_POSITION | ATTR_NORMAL | ATTR_UV | ATTR_TANGENT
        );
        assert_eq!(u32_at(tan_payload, 4), STRIDE_WITH_TANGENT);
        // Three vertices, each 16 bytes wider (the 4×f32 tangent), and nothing else moved.
        assert_eq!(tan_payload.len(), plain_payload.len() + 3 * 16);
    }

    #[test]
    fn a_skin_sets_the_joint_weight_flags_and_widens_the_stride() {
        use crate::cooked::read_header;
        let mut mesh = Mesh {
            vertices: vec![
                Vertex {
                    position: [0.0, 0.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 0.0],
                },
                Vertex {
                    position: [1.0, 0.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [1.0, 0.0],
                },
                Vertex {
                    position: [0.0, 1.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 1.0],
                },
            ],
            indices: vec![0, 1, 2],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 3,
                material_slot: 0,
            }],
            tangents: None,
            skin: None,
        };
        let (plain, _) = mesh.cook();
        let (_, plain_payload) = read_header(&plain).unwrap();

        mesh.skin = Some(SkinWeights {
            joints: vec![[0, 1, 2, 3]; 3],
            weights: vec![[0.5, 0.25, 0.15, 0.1]; 3],
        });
        let (skinned, _) = mesh.cook();
        let (_, skinned_payload) = read_header(&skinned).unwrap();
        assert_eq!(
            u32_at(skinned_payload, 0),
            ATTR_POSITION | ATTR_NORMAL | ATTR_UV | ATTR_JOINTS | ATTR_WEIGHTS
        );
        assert_eq!(u32_at(skinned_payload, 4), STRIDE_NO_TANGENT + SKIN_BYTES); // 32 + 24 = 56
                                                                                // Each vertex grew by 4×u16 joints + 4×f32 weights = 24 bytes; nothing else moved.
        assert_eq!(
            skinned_payload.len(),
            plain_payload.len() + 3 * SKIN_BYTES as usize
        );
    }

    #[test]
    fn weights_renormalize_to_sum_one() {
        // Weights that sum to 2 halve; a partial set (summing to 0.5) scales up to 1.
        let n = normalize_weights([0.5, 0.5, 0.5, 0.5]);
        assert!((n.iter().sum::<f32>() - 1.0).abs() < 1e-6);
        assert!((n[0] - 0.25).abs() < 1e-6);
        let m = normalize_weights([0.25, 0.25, 0.0, 0.0]);
        assert!((m.iter().sum::<f32>() - 1.0).abs() < 1e-6);
        assert!((m[0] - 0.5).abs() < 1e-6);
        // A vertex with no influence is pinned fully to its first joint, not left weightless.
        assert_eq!(
            normalize_weights([0.0, 0.0, 0.0, 0.0]),
            [1.0, 0.0, 0.0, 0.0]
        );
    }

    fn u32_at(bytes: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
    }
}
