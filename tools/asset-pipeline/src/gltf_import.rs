// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! glTF 2.0 mesh import. Walks the scene's node hierarchy, **flattens each node's world transform
//! into its vertices** (M6.2 keeps no runtime scene graph from glTF), and pulls positions, normals,
//! UVs, and indices for every triangle primitive. glTF is *an* importer, not the format: the RMA1
//! cooked layout (ADR-0024) is the contract, and STL joins as a second importer at M6.6.
//!
//! Out of scope for v1 (rejected or defaulted with a clear message): non-triangle primitive modes,
//! Draco-compressed geometry (no POSITION reachable), skins/materials/tangents (later bricks).

use std::path::Path;

use crate::math::{self, Mat4};
use crate::mesh::{self, Primitive, Vertex};
use crate::PipelineError;

/// Import every triangle primitive in the file's default scene, each already flattened to world
/// space. The returned order is a stable depth-first walk, so cooking is deterministic.
pub fn import_primitives(path: &Path) -> Result<Vec<Primitive>, PipelineError> {
    let (document, buffers, _images) = gltf::import(path)?;
    let scene = document
        .default_scene()
        .or_else(|| document.scenes().next())
        .ok_or_else(|| PipelineError::Unsupported("glTF has no scene".to_string()))?;

    let mut out = Vec::new();
    for node in scene.nodes() {
        walk_node(&node, math::IDENTITY, &buffers, &mut out)?;
    }
    Ok(out)
}

fn walk_node(
    node: &gltf::Node,
    parent_world: Mat4,
    buffers: &[gltf::buffer::Data],
    out: &mut Vec<Primitive>,
) -> Result<(), PipelineError> {
    // glTF gives each node's local transform as a column-major matrix; accumulate down the tree.
    let world = math::mul(
        &parent_world,
        &math::from_columns(node.transform().matrix()),
    );

    if let Some(mesh) = node.mesh() {
        for primitive in mesh.primitives() {
            out.push(import_primitive(&primitive, &world, buffers)?);
        }
    }
    for child in node.children() {
        walk_node(&child, world, buffers, out)?;
    }
    Ok(())
}

fn import_primitive(
    primitive: &gltf::Primitive,
    world: &Mat4,
    buffers: &[gltf::buffer::Data],
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

    let world_positions: Vec<[f32; 3]> = positions
        .iter()
        .map(|&p| math::transform_point(world, p))
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
        Some(read) => read.map(|n| math::transform_normal(world, n)).collect(),
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

    Ok(Primitive { vertices, indices })
}
