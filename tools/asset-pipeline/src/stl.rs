// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Binary STL import → cooked RMA1 mesh (M6.6). STL is the interchange format ICEM writes its
//! computed engineering parts as; `samples/03-icem-viewer/stl.hpp` is the viewer's own in-process
//! reader. This module graduates that loader *pattern* into the offline pipeline (ADR-0016 rule 3),
//! so those parts cook to the same `MeshAsset` the engine already loads — proving the pipeline is not
//! glTF-shaped.
//!
//! We import a *flat-shaded* mesh: each triangle's three vertices carry that triangle's own geometric
//! face normal, computed **bit-faithfully to the viewer's loader** so a cooked part shades identically
//! to the live-loaded one (the M6.6 pixel-identity dogfood). STL is an un-indexed triangle soup, so we
//! then dedup exact-equal vertices into an index buffer — a shared vertex written once per triangle
//! that touches it collapses to one, which is the size win an indexed cooked mesh buys.

use std::collections::HashMap;

use crate::math;
use crate::mesh::{Mesh, Submesh, Vertex};
use crate::PipelineError;

const STL_HEADER: usize = 80;
const STL_TRI_BYTES: usize = 50; // 12 f32 (a face normal + three vertices) + a 2-byte attribute word

fn read_f32(data: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]])
}

fn read_vec3(data: &[u8], off: usize) -> [f32; 3] {
    [
        read_f32(data, off),
        read_f32(data, off + 4),
        read_f32(data, off + 8),
    ]
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn length(v: [f32; 3]) -> f32 {
    (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt()
}

/// The flat-shaded face normal for one triangle, computed exactly as the viewer's loader does
/// (`samples/03-icem-viewer/stl.hpp`): the geometric normal `cross(v1-v0, v2-v0)` divided by its own
/// length — scale-independent, with **no** epsilon zero-guard, so the legitimately tiny cross products
/// of a finely-meshed metre-scale part are not wrongly nulled to a black-shading NaN. Only a truly
/// zero-area triangle falls back to the file's stored normal, and a still-zero result to `+Z`. Keeping
/// this identical to the viewer is what lets the cooked mesh render pixel-for-pixel like the live one.
fn face_normal(v0: [f32; 3], v1: [f32; 3], v2: [f32; 3], file_normal: [f32; 3]) -> [f32; 3] {
    let mut n = math::cross(sub(v1, v0), sub(v2, v0));
    let mut nlen = length(n);
    if nlen <= 0.0 {
        n = file_normal; // degenerate triangle: trust the file's stored normal
        nlen = length(n);
    }
    if nlen > 0.0 {
        [n[0] / nlen, n[1] / nlen, n[2] / nlen]
    } else {
        [0.0, 0.0, 1.0]
    }
}

/// The outcome of an STL import: the indexed mesh plus the soup-vs-unique vertex counts, so the caller
/// (and the proof) can report the dedup size win.
#[derive(Debug)]
pub struct StlImport {
    pub mesh: Mesh,
    pub triangle_count: u32,
    pub soup_vertex_count: u32, // 3 * triangles — the un-indexed soup, before dedup
    pub unique_vertex_count: u32, // after exact position+normal dedup
}

/// Parse a binary STL from memory into an indexed, flat-shaded [`Mesh`]. Errors (as
/// `PipelineError::Unsupported`) on a buffer too small to be binary STL, a zero-triangle file, a
/// triangle count that disagrees with the byte length, or an ASCII STL (which v1 does not cook).
pub fn import_stl_binary(data: &[u8]) -> Result<StlImport, PipelineError> {
    // Binary STL is exactly `80-byte header | u32 count | count * 50 bytes` (trailing padding allowed).
    // ASCII STL is text beginning with "solid " — its bytes 80..84 are not a real triangle count, so it
    // fails the length check below; we detect the "solid" prefix there and reject it with a clear
    // message rather than misreading text as little-endian floats. (ICEM's binary writer deliberately
    // does not start its header with "solid", so a valid binary file never trips the ASCII branch.)
    let looks_ascii = data.starts_with(b"solid");
    let ascii_err = || {
        PipelineError::Unsupported(
            "ASCII STL is not supported (v1 cooks binary STL only) — re-export as binary STL"
                .into(),
        )
    };
    if data.len() < STL_HEADER + 4 {
        return Err(if looks_ascii {
            ascii_err()
        } else {
            PipelineError::Unsupported("STL shorter than an 80-byte header + triangle count".into())
        });
    }

    let tri_count = u32::from_le_bytes([data[80], data[81], data[82], data[83]]) as usize;
    let need = STL_HEADER + 4 + tri_count * STL_TRI_BYTES;
    if tri_count == 0 || data.len() < need {
        return Err(if looks_ascii {
            ascii_err()
        } else {
            PipelineError::Unsupported(format!(
                "binary STL declares {tri_count} triangles but the file has {} bytes (needs {need})",
                data.len()
            ))
        });
    }

    // Dedup exact-equal vertices (position AND normal bit-identical) into an index buffer. Two triangles
    // that share an edge within one flat face share both position and face normal, so they collapse;
    // across a crease the face normals differ, so the seam vertices stay distinct and the faceting is
    // preserved exactly. The key is the raw f32 bits, so the merge is lossless — no epsilon that could
    // shift a vertex and change a pixel — which means the indexed mesh index-expands back to the exact
    // original soup, the property the viewer dogfood relies on.
    let mut vertices: Vec<Vertex> = Vec::new();
    let mut indices: Vec<u32> = Vec::with_capacity(tri_count * 3);
    let mut seen: HashMap<[u32; 6], u32> = HashMap::new();
    for i in 0..tri_count {
        let base = STL_HEADER + 4 + i * STL_TRI_BYTES;
        let file_n = read_vec3(data, base);
        let v0 = read_vec3(data, base + 12);
        let v1 = read_vec3(data, base + 24);
        let v2 = read_vec3(data, base + 36);
        let n = face_normal(v0, v1, v2, file_n);
        for p in [v0, v1, v2] {
            let key = [
                p[0].to_bits(),
                p[1].to_bits(),
                p[2].to_bits(),
                n[0].to_bits(),
                n[1].to_bits(),
                n[2].to_bits(),
            ];
            let index = *seen.entry(key).or_insert_with(|| {
                let id = vertices.len() as u32;
                vertices.push(Vertex {
                    position: p,
                    normal: n,
                    uv: [0.0, 0.0], // STL carries no texture coordinates
                });
                id
            });
            indices.push(index);
        }
    }

    let soup_vertex_count = (tri_count * 3) as u32;
    let unique_vertex_count = vertices.len() as u32;
    let index_count = indices.len() as u32;
    let mesh = Mesh {
        submeshes: vec![Submesh {
            first_index: 0,
            index_count,
            material_slot: 0,
        }],
        vertices,
        indices,
        tangents: None,
        skin: None, // STL has no rig; a skinned mesh comes only from glTF
    };
    Ok(StlImport {
        mesh,
        triangle_count: tri_count as u32,
        soup_vertex_count,
        unique_vertex_count,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Assemble a binary STL from `(face_normal, [v0, v1, v2])` triangles — a test-only writer mirroring
    /// what ICEM emits, so the importer is exercised against real bytes, not a mocked struct.
    fn build_stl(triangles: &[([f32; 3], [[f32; 3]; 3])]) -> Vec<u8> {
        let mut b = vec![0u8; STL_HEADER]; // 80-byte header, all zero (not "solid...")
        b.extend_from_slice(&(triangles.len() as u32).to_le_bytes());
        for (normal, verts) in triangles {
            for c in normal {
                b.extend_from_slice(&c.to_le_bytes());
            }
            for v in verts {
                for c in v {
                    b.extend_from_slice(&c.to_le_bytes());
                }
            }
            b.extend_from_slice(&0u16.to_le_bytes()); // attribute byte count
        }
        b
    }

    #[test]
    fn two_triangles_sharing_an_edge_dedup_to_four_vertices() {
        // A unit quad in the z=0 plane, split into two triangles that share the v0..v2 diagonal. Both
        // triangles are coplanar, so they share the same +Z face normal — the two shared corners must
        // collapse: 6 soup vertices → 4 unique. The stored file normal is deliberately wrong ([0,0,-1])
        // to prove we recompute geometrically rather than trusting it.
        let a = [0.0, 0.0, 0.0];
        let b = [1.0, 0.0, 0.0];
        let c = [1.0, 1.0, 0.0];
        let d = [0.0, 1.0, 0.0];
        let bytes = build_stl(&[([0.0, 0.0, -1.0], [a, b, c]), ([0.0, 0.0, -1.0], [a, c, d])]);

        let imp = import_stl_binary(&bytes).unwrap();
        assert_eq!(imp.triangle_count, 2);
        assert_eq!(imp.soup_vertex_count, 6);
        assert_eq!(imp.unique_vertex_count, 4); // the shared diagonal a & c merged
        assert_eq!(imp.mesh.indices.len(), 6);
        // Recomputed normal is geometric +Z, not the file's bogus -Z.
        for v in &imp.mesh.vertices {
            assert!(
                (v.normal[2] - 1.0).abs() < 1e-6,
                "normal should be +Z, got {:?}",
                v.normal
            );
        }
        // The index buffer expands back to the exact original soup (positions in triangle order).
        let soup: Vec<[f32; 3]> = imp
            .mesh
            .indices
            .iter()
            .map(|&i| imp.mesh.vertices[i as usize].position)
            .collect();
        assert_eq!(soup, vec![a, b, c, a, c, d]);
    }

    #[test]
    fn a_crease_keeps_its_seam_vertices_distinct() {
        // Two triangles meeting at a right-angle crease along the shared edge. Same positions on the
        // seam, but different face normals — so the seam vertices must NOT merge (faceting preserved).
        let p0 = [0.0, 0.0, 0.0];
        let p1 = [1.0, 0.0, 0.0];
        let up = [0.0, 1.0, 0.0]; // +Z face uses this? build two non-coplanar tris sharing edge p0-p1
        let down = [0.0, 0.0, 1.0];
        let bytes = build_stl(&[([0.0; 3], [p0, p1, up]), ([0.0; 3], [p1, p0, down])]);
        let imp = import_stl_binary(&bytes).unwrap();
        // 6 soup verts; p0 and p1 appear in both tris but with different normals → no merge → 6 unique.
        assert_eq!(imp.unique_vertex_count, 6);
    }

    #[test]
    fn ascii_stl_is_rejected_clearly() {
        let ascii = b"solid teapot\n facet normal 0 0 1\n".to_vec();
        let err = import_stl_binary(&ascii).unwrap_err();
        assert!(
            matches!(err, PipelineError::Unsupported(ref m) if m.contains("ASCII")),
            "got {err}"
        );
    }

    #[test]
    fn a_truncated_binary_stl_is_rejected() {
        // Declares 3 triangles but supplies bytes for none.
        let mut bytes = vec![0u8; STL_HEADER];
        bytes.extend_from_slice(&3u32.to_le_bytes());
        assert!(import_stl_binary(&bytes).is_err());
    }
}
