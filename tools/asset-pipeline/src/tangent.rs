// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Per-vertex tangent generation for normal mapping (M6.4). A normal map stores its perturbations in
//! *tangent space* — a per-fragment basis (T, B, N) glued to the surface — so to apply one the engine
//! needs a tangent at every vertex, not just a normal. glTF lets a mesh omit tangents and, when it
//! does, specifies that they be generated with **MikkTSpace** (Morten Mikkelsen's algorithm). We use
//! the `mikktspace` crate so our basis matches the one normal maps are authored against; the full
//! derivation (why per-vertex tangents, the handedness sign, the TBN reconstruction) lives in
//! `docs/math/tangent-space.md`.
//!
//! The result per vertex is a `[f32; 4]`: the unit tangent in `xyz` and a **handedness sign** in `w`.
//! The engine reconstructs the bitangent as `w · cross(N, T)` (the glTF convention) rather than
//! storing it — half the bandwidth, and it stays correct under mirrored UVs, which is exactly what the
//! sign encodes. A left-handed UV chart (a mirrored shell, common on symmetric models) flips the sign;
//! getting that wrong lights the mirror seam inside-out — the classic MikkTSpace trap the handedness
//! test below pins.
//!
//! **v1 simplification (documented):** MikkTSpace can *split* a shared vertex when two faces disagree
//! on its tangent (a hard tangent seam), which would grow the vertex count. We keep the mesh indexed
//! and write each face-vertex's tangent back onto its shared vertex, so a vertex touched by disagreeing
//! faces keeps the last one visited. For our meshes (smooth normals, continuous UVs) the faces agree
//! and this is exact; the de-index/re-weld that makes hard seams exact is a noted seam, not needed
//! until a model shows the artifact.

use crate::mesh::{Mesh, Vertex};

/// Fill `mesh.tangents` with a MikkTSpace tangent per vertex (unit `xyz` + handedness `w`). Returns
/// `false` if generation failed (a degenerate mesh — no faces, or UVs so degenerate no basis exists),
/// in which case `mesh.tangents` is left `None` and the mesh cooks without the tangent attribute.
///
/// Only call this for meshes whose material actually needs a normal map (the M6.4 policy): tangents
/// cost a wider vertex, and a mesh with no UVs has no meaningful tangent to generate.
pub fn generate_tangents(mesh: &mut Mesh) -> bool {
    if mesh.indices.is_empty() || !mesh.indices.len().is_multiple_of(3) {
        return false;
    }
    let mut geom = TangentGeometry {
        vertices: &mesh.vertices,
        indices: &mesh.indices,
        // Seed with the identity tangent so a vertex MikkTSpace never visits (an orphan not in any
        // face) still decodes to a finite, unit basis rather than zeros.
        tangents: vec![[1.0, 0.0, 0.0, 1.0]; mesh.vertices.len()],
    };
    if !mikktspace::generate_tangents(&mut geom) {
        return false;
    }
    mesh.tangents = Some(geom.tangents);
    true
}

/// The adapter that presents our indexed triangle list to MikkTSpace as a face/vertex callback
/// surface. MikkTSpace pulls geometry through `position`/`normal`/`tex_coord` and pushes each
/// face-vertex's computed tangent back through `set_tangent_encoded`.
struct TangentGeometry<'a> {
    vertices: &'a [Vertex],
    indices: &'a [u32],
    tangents: Vec<[f32; 4]>,
}

impl TangentGeometry<'_> {
    /// The mesh-global vertex index behind face `face`'s corner `vert` — the indirection that keeps
    /// us indexed instead of a de-indexed triangle soup.
    fn index(&self, face: usize, vert: usize) -> usize {
        self.indices[face * 3 + vert] as usize
    }
}

impl mikktspace::Geometry for TangentGeometry<'_> {
    fn num_faces(&self) -> usize {
        self.indices.len() / 3
    }

    // Triangle list: every face has exactly three vertices.
    fn num_vertices_of_face(&self, _face: usize) -> usize {
        3
    }

    fn position(&self, face: usize, vert: usize) -> [f32; 3] {
        self.vertices[self.index(face, vert)].position
    }

    fn normal(&self, face: usize, vert: usize) -> [f32; 3] {
        self.vertices[self.index(face, vert)].normal
    }

    fn tex_coord(&self, face: usize, vert: usize) -> [f32; 2] {
        self.vertices[self.index(face, vert)].uv
    }

    // MikkTSpace hands us the tangent already encoded as `[x, y, z, sign]`, with `sign` the bitangent
    // handedness in the glTF convention (bitangent = sign · cross(N, T)). Write it back onto the
    // shared vertex (see the module note on last-write-wins for hard seams).
    fn set_tangent_encoded(&mut self, tangent: [f32; 4], face: usize, vert: usize) {
        // Resolve the shared-vertex index before the mutable borrow of `self.tangents` (a single
        // `self.tangents[self.index(..)]` would borrow `self` both ways at once).
        let index = self.index(face, vert);
        self.tangents[index] = tangent;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mesh::Vertex;

    // A single +Z-facing quad with a standard UV chart: u grows with +x, v grows with +y. Two CCW
    // triangles. This is the canonical tangent-space test body — the analytic answer is known.
    fn quad_plus_z() -> Mesh {
        let vertices = vec![
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
                position: [1.0, 1.0, 0.0],
                normal: [0.0, 0.0, 1.0],
                uv: [1.0, 1.0],
            },
            Vertex {
                position: [0.0, 1.0, 0.0],
                normal: [0.0, 0.0, 1.0],
                uv: [0.0, 1.0],
            },
        ];
        Mesh {
            vertices,
            indices: vec![0, 1, 2, 0, 2, 3],
            ..Default::default()
        }
    }

    fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
        [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0],
        ]
    }

    #[test]
    fn tangents_are_finite_and_unit_length() {
        let mut mesh = quad_plus_z();
        assert!(generate_tangents(&mut mesh));
        let tangents = mesh.tangents.as_ref().unwrap();
        assert_eq!(tangents.len(), mesh.vertices.len());
        for t in tangents {
            for c in t {
                assert!(c.is_finite(), "tangent component must be finite, got {t:?}");
            }
            let len = (t[0] * t[0] + t[1] * t[1] + t[2] * t[2]).sqrt();
            assert!(
                (len - 1.0).abs() < 1e-3,
                "tangent xyz must be unit, got |{t:?}| = {len}"
            );
            assert!(
                t[3] == 1.0 || t[3] == -1.0,
                "handedness must be ±1, got {}",
                t[3]
            );
        }
    }

    #[test]
    fn generation_is_deterministic() {
        let mut a = quad_plus_z();
        let mut b = quad_plus_z();
        assert!(generate_tangents(&mut a));
        assert!(generate_tangents(&mut b));
        assert_eq!(
            a.tangents, b.tangents,
            "tangent generation must be deterministic"
        );
    }

    #[test]
    fn tangent_points_along_increasing_u_and_the_handedness_matches_gltf() {
        // For this chart the tangent (∂p/∂u) is +x and the bitangent (∂p/∂v) is +y. The engine
        // reconstructs B = w · cross(N, T); with N = +z and T ≈ +x, cross(N, T) = +y, so the sign
        // that makes B = +y (the actual ∂p/∂v) is w = +1. This pins BOTH that MikkTSpace agrees with
        // the analytic tangent AND that its handedness sign is the glTF convention we decode against.
        let mut mesh = quad_plus_z();
        assert!(generate_tangents(&mut mesh));
        let tangents = mesh.tangents.as_ref().unwrap();
        for t in tangents {
            assert!(
                t[0] > 0.9,
                "tangent should point along +x (increasing u), got {t:?}"
            );
            assert!(
                t[1].abs() < 0.1 && t[2].abs() < 0.1,
                "tangent should be ~+x, got {t:?}"
            );

            // Reconstruct the bitangent the way the shader does and check it points along +v (+y).
            let n = [0.0, 0.0, 1.0];
            let txyz = [t[0], t[1], t[2]];
            let b = cross(n, txyz);
            let by = b[1] * t[3];
            assert!(
                by > 0.9,
                "w · cross(N,T) must point +y (increasing v); w={} gave {by}",
                t[3]
            );
        }
    }

    #[test]
    fn a_degenerate_index_buffer_is_rejected_cleanly() {
        let mut mesh = quad_plus_z();
        mesh.indices = vec![0, 1]; // not a whole triangle
        assert!(!generate_tangents(&mut mesh));
        assert!(mesh.tangents.is_none());
    }
}
