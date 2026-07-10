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
/// position|normal|uv; a normal-mapped mesh (M6.4) adds `tangent` and a wider stride.
pub const ATTR_POSITION: u32 = 1 << 0;
pub const ATTR_NORMAL: u32 = 1 << 1;
pub const ATTR_UV: u32 = 1 << 2;
pub const ATTR_TANGENT: u32 = 1 << 3;

/// Interleaved vertex stride without / with tangents (bytes): pos+normal+uv is 32, +tangent is 48.
/// These mirror `expected_vertex_stride` in the engine's `mesh_asset.hpp`; the reader validates the
/// declared stride against the attribute flags, so a corrupt stride can't walk vertex addressing off
/// the blob.
pub const STRIDE_NO_TANGENT: u32 = 32;
pub const STRIDE_WITH_TANGENT: u32 = 48;

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
}

impl Mesh {
    /// Merge several world-space primitives into one mesh: vertices are concatenated, each
    /// primitive's indices are rebased onto its vertex block, and each becomes one submesh. This is
    /// how a glTF file's primitives (already flattened to world space at import) become a single
    /// cooked mesh with a submesh table.
    pub fn from_primitives(primitives: Vec<Primitive>) -> Self {
        let mut mesh = Mesh::default();
        for prim in primitives {
            let base = mesh.vertices.len() as u32;
            let first_index = mesh.indices.len() as u32;
            mesh.vertices.extend(prim.vertices);
            mesh.indices.extend(prim.indices.iter().map(|&i| i + base));
            mesh.submeshes.push(Submesh {
                first_index,
                index_count: prim.indices.len() as u32,
                material_slot: 0,
            });
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
        // Tangents are optional and additive: their presence sets one more attribute flag and widens
        // the stride by a 4×f32 tangent, with no new container version. The declared stride and
        // flags travel together, and the C++ reader re-derives the stride from the flags — so a
        // tangent-bearing mesh and a plain one differ only in these two header words plus the extra
        // per-vertex bytes.
        let has_tangents = self.tangents.is_some();
        let (attribs, stride) = if has_tangents {
            (
                ATTR_POSITION | ATTR_NORMAL | ATTR_UV | ATTR_TANGENT,
                STRIDE_WITH_TANGENT,
            )
        } else {
            (ATTR_POSITION | ATTR_NORMAL | ATTR_UV, STRIDE_NO_TANGENT)
        };
        // The tangent array, when present, is one-per-vertex by construction (generate_tangents sizes
        // it to vertices.len()). Assert it, so a hand-built mesh can't cook a blob whose stride and
        // vertex count silently disagree — the reader would reject it, but failing here names why.
        debug_assert!(
            self.tangents
                .as_ref()
                .is_none_or(|t| t.len() == self.vertices.len()),
            "tangent array must have exactly one entry per vertex"
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
            // Tangent last, matching the attribute order the reader's `expected_vertex_stride` walks.
            if let Some(tangents) = &self.tangents {
                for c in tangents[i] {
                    p.f32(c);
                }
            }
        }
        for &i in &self.indices {
            p.u32(i);
        }
        wrap_container(ASSET_KIND_MESH, MESH_SCHEMA_HASH, &p.into_vec())
    }
}

/// One imported primitive in world space, before merging. `indices` are local to `vertices`.
pub struct Primitive {
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u32>,
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

    fn u32_at(bytes: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
    }
}
