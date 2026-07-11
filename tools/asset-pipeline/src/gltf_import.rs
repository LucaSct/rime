// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! glTF 2.0 mesh import. Walks the scene's node hierarchy, **flattens each node's world transform
//! into its vertices** (M6.2 keeps no runtime scene graph from glTF), and pulls positions, normals,
//! UVs, and indices for every triangle primitive. glTF is *an* importer, not the format: the RMA1
//! cooked layout (ADR-0024) is the contract, and STL joins as a second importer at M6.6.
//!
//! This module imports *geometry* only. From M6.4 it also tags each primitive with its material
//! slot (the index into the cooked model's material table), but the material *data* — factors and
//! texture references — is imported by the sibling `gltf_material` module, and tangents are generated
//! by `tangent`. Out of scope for v1 (rejected or defaulted with a clear message): non-triangle
//! primitive modes, Draco-compressed geometry (no POSITION reachable), skins (a later brick).

use std::path::Path;

use crate::math::{self, Mat4};
use crate::mesh::{self, normalize_weights, Primitive, SkinWeights, Vertex};
use crate::PipelineError;

/// Import every triangle primitive in the file's default scene, each already flattened to world
/// space. A convenience wrapper that imports the glTF and delegates to `primitives_from`; the
/// cooker's `cook_gltf` imports the document once and calls `primitives_from` directly, sharing the
/// decode with the material import.
pub fn import_primitives(path: &Path) -> Result<Vec<Primitive>, PipelineError> {
    let (document, buffers, _images) = gltf::import(path)?;
    primitives_from(&document, &buffers)
}

/// Walk an already-imported document's default scene into world-space primitives. The returned order
/// is a stable depth-first walk, so cooking is deterministic.
pub(crate) fn primitives_from(
    document: &gltf::Document,
    buffers: &[gltf::buffer::Data],
) -> Result<Vec<Primitive>, PipelineError> {
    let scene = document
        .default_scene()
        .or_else(|| document.scenes().next())
        .ok_or_else(|| PipelineError::Unsupported("glTF has no scene".to_string()))?;

    // A primitive with no material renders with glTF's default material, which the cooker appends
    // just past the defined materials — so its slot is the count of defined materials (M6.4).
    let material_count = document.materials().count() as u32;

    let mut out = Vec::new();
    for node in scene.nodes() {
        walk_node(&node, math::IDENTITY, buffers, material_count, &mut out)?;
    }
    Ok(out)
}

fn walk_node(
    node: &gltf::Node,
    parent_world: Mat4,
    buffers: &[gltf::buffer::Data],
    material_count: u32,
    out: &mut Vec<Primitive>,
) -> Result<(), PipelineError> {
    // glTF gives each node's local transform as a column-major matrix; accumulate down the tree.
    let world = math::mul(
        &parent_world,
        &math::from_columns(node.transform().matrix()),
    );

    if let Some(mesh) = node.mesh() {
        // A node with a skin drives its mesh by the skeleton, not by its own transform: glTF requires
        // the mesh node's transform be ignored for a skinned mesh (the joints, with their inverse
        // binds, place every vertex). So skinned primitives import in bind space, un-flattened.
        let is_skinned = node.skin().is_some();
        for primitive in mesh.primitives() {
            out.push(import_primitive(
                &primitive,
                &world,
                is_skinned,
                buffers,
                material_count,
            )?);
        }
    }
    for child in node.children() {
        walk_node(&child, world, buffers, material_count, out)?;
    }
    Ok(())
}

fn import_primitive(
    primitive: &gltf::Primitive,
    world: &Mat4,
    is_skinned: bool,
    buffers: &[gltf::buffer::Data],
    material_count: u32,
) -> Result<Primitive, PipelineError> {
    if primitive.mode() != gltf::mesh::Mode::Triangles {
        return Err(PipelineError::Unsupported(format!(
            "primitive mode {:?} — only triangle lists are cooked in v1",
            primitive.mode()
        )));
    }

    let reader = primitive.reader(|b| buffers.get(b.index()).map(|d| d.0.as_slice()));

    // POSITION is required; its absence usually means Draco/compressed geometry we do not decode.
    let positions: Vec<[f32; 3]> = reader
        .read_positions()
        .ok_or_else(|| {
            PipelineError::Unsupported(
                "primitive has no POSITION accessor (Draco/compressed geometry?)".to_string(),
            )
        })?
        .collect();

    // Static primitives flatten their node's world transform into the vertices (M6.2 keeps no runtime
    // scene graph); skinned primitives stay in bind space (identity) so their JOINTS_0 indices and the
    // skeleton's inverse-bind matrices line up at sample time.
    let vertex_xform: &Mat4 = if is_skinned { &math::IDENTITY } else { world };
    let world_positions: Vec<[f32; 3]> = positions
        .iter()
        .map(|&p| math::transform_point(vertex_xform, p))
        .collect();

    // Indices: a triangle list. If the primitive is non-indexed, synthesize 0..n. u8/u16 promote to
    // u32 via the reader's `into_u32()`.
    let indices: Vec<u32> = match reader.read_indices() {
        Some(read) => read.into_u32().collect(),
        None => (0..world_positions.len() as u32).collect(),
    };

    if world_positions.is_empty() || indices.is_empty() || !indices.len().is_multiple_of(3) {
        return Err(PipelineError::Unsupported(
            "primitive is empty or not a whole number of triangles".to_string(),
        ));
    }

    // Normals: transform if present (covector → inverse-transpose), else derive from the geometry.
    let normals: Vec<[f32; 3]> = match reader.read_normals() {
        Some(read) => read
            .map(|n| math::transform_normal(vertex_xform, n))
            .collect(),
        None => mesh::compute_normals(&world_positions, &indices),
    };

    // UVs: default to (0,0) if the primitive has none (materials/texturing arrive at M6.3/M6.4).
    let uvs: Vec<[f32; 2]> = match reader.read_tex_coords(0) {
        Some(read) => read.into_f32().collect(),
        None => vec![[0.0, 0.0]; world_positions.len()],
    };

    if normals.len() != world_positions.len() || uvs.len() != world_positions.len() {
        return Err(PipelineError::Unsupported(
            "vertex attribute counts disagree".to_string(),
        ));
    }

    let vertices: Vec<Vertex> = world_positions
        .iter()
        .zip(&normals)
        .zip(&uvs)
        .map(|((&position, &normal), &uv)| Vertex {
            position,
            normal,
            uv,
        })
        .collect();

    // The material slot: a defined material's own index, or the appended default-material slot (just
    // past the defined materials) when this primitive names none. The cooked bytes carry only this
    // slot; `gltf_material` cooks the material data it points at.
    let material_slot = primitive
        .material()
        .index()
        .map(|i| i as u32)
        .unwrap_or(material_count);

    // Skin binding: for a skinned primitive, the four joint indices and weights per vertex. glTF's
    // JOINTS_0/WEIGHTS_0 hold the first (and, for v1, only) four influences — a second set (JOINTS_1)
    // is dropped with a warning and the surviving four renormalized. The joint indices reference the
    // node's skin joint list, which the skeleton cooker preserves, so no remapping is needed here.
    let skin = if is_skinned {
        if reader.read_joints(1).is_some() {
            eprintln!(
                "warning: primitive has more than 4 joint influences per vertex; \
                 dropping the extras (JOINTS_1+) and renormalizing (v1 skins at most 4)"
            );
        }
        let joints: Vec<[u16; 4]> = reader
            .read_joints(0)
            .ok_or_else(|| {
                PipelineError::Unsupported("skinned primitive has no JOINTS_0 accessor".to_string())
            })?
            .into_u16()
            .collect();
        let weights: Vec<[f32; 4]> = reader
            .read_weights(0)
            .ok_or_else(|| {
                PipelineError::Unsupported(
                    "skinned primitive has no WEIGHTS_0 accessor".to_string(),
                )
            })?
            .into_f32()
            .map(normalize_weights)
            .collect();
        if joints.len() != world_positions.len() || weights.len() != world_positions.len() {
            return Err(PipelineError::Unsupported(
                "skin joint/weight counts disagree with the vertex count".to_string(),
            ));
        }
        Some(SkinWeights { joints, weights })
    } else {
        None
    };

    Ok(Primitive {
        vertices,
        indices,
        material_slot,
        skin,
    })
}
