// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Integration tests for the M6.2 glTF mesh cooker, exercised against the shared fixtures in
//! `tests/assets/fixtures/`. The committed `quad.rmesh` is the file the C++ `engine/assets` test
//! reads, so the `quad_cooks_to_the_committed_fixture_bytes` case is the cross-language drift alarm:
//! if the cooked format changes, this fails until the fixture is regenerated (`rime cook
//! tests/assets/fixtures/quad.gltf --out tests/assets/fixtures`).

use std::path::{Path, PathBuf};

use asset_pipeline::gltf_import;
use asset_pipeline::mesh::Mesh;
use asset_pipeline::texture::{ColorSpace, Texture};

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/assets/fixtures")
}

fn cook(name: &str) -> Vec<u8> {
    let primitives = gltf_import::import_primitives(&fixtures().join(name)).unwrap();
    Mesh::from_primitives(primitives).cook().0
}

#[test]
fn quad_cooks_to_the_committed_fixture_bytes() {
    let committed = std::fs::read(fixtures().join("quad.rmesh")).unwrap();
    assert_eq!(
        cook("quad.gltf"),
        committed,
        "cooker output diverged from the committed fixture — regenerate it deliberately"
    );
}

#[test]
fn cook_is_byte_stable() {
    assert_eq!(cook("quad.gltf"), cook("quad.gltf"));
}

#[test]
fn quad_import_flattens_the_node_translation() {
    let mesh = Mesh::from_primitives(
        gltf_import::import_primitives(&fixtures().join("quad.gltf")).unwrap(),
    );
    assert_eq!(mesh.vertices.len(), 4);
    assert_eq!(mesh.indices, vec![0, 1, 2, 0, 2, 3]); // u16 source, promoted to u32
    assert_eq!(mesh.submeshes.len(), 1);
    assert_eq!(mesh.submeshes[0].index_count, 6);

    // The node is translated by (1,2,3); the AABB proves it was flattened into the vertices.
    let (min, max) = mesh.aabb();
    assert_eq!(min, [1.0, 2.0, 3.0]);
    assert_eq!(max, [2.0, 3.0, 3.0]);
    // A pure translation leaves normals unchanged.
    for v in &mesh.vertices {
        assert_eq!(v.normal, [0.0, 0.0, 1.0]);
    }
}

#[test]
fn checker_png_cooks_to_the_committed_texture_fixture_bytes() {
    // The M6.3 texture cross-language alarm, mirroring the mesh one above: the committed
    // checker.rtex is what the C++ engine/assets test reads. If the cooked texture format — header,
    // mip table, or the *gamma-correct* mip pixels — ever changes, this fails until the fixture is
    // regenerated deliberately (`rime cook tests/assets/fixtures/checker.png --out
    // tests/assets/fixtures`). checker.png is a 2×2 sRGB checker; sRGB is the cook default.
    let committed = std::fs::read(fixtures().join("checker.rtex")).unwrap();
    let cooked = Texture::from_file(&fixtures().join("checker.png"), ColorSpace::Srgb)
        .unwrap()
        .cook()
        .0;
    assert_eq!(
        cooked, committed,
        "texture cooker output diverged from the committed fixture — regenerate it deliberately"
    );
}

#[test]
fn committed_checker_fixture_carries_the_gamma_correct_mip() {
    // A file-level spot-check of the headline behaviour: the fixture's 1×1 mip (its last four bytes)
    // is sRGB 188 — linear 0.5 re-encoded — not 128, the too-dark naive sRGB-space average.
    let bytes = std::fs::read(fixtures().join("checker.rtex")).unwrap();
    let mip1 = &bytes[bytes.len() - 4..];
    assert_eq!(mip1, [188, 188, 188, 255]);
}

#[test]
fn missing_normals_are_computed_from_geometry() {
    // quad_flat.gltf omits the NORMAL accessor: the cooker derives +Z for the planar quad.
    let mesh = Mesh::from_primitives(
        gltf_import::import_primitives(&fixtures().join("quad_flat.gltf")).unwrap(),
    );
    assert_eq!(mesh.vertices.len(), 4);
    for v in &mesh.vertices {
        assert!(
            v.normal[0].abs() < 1e-6
                && v.normal[1].abs() < 1e-6
                && (v.normal[2] - 1.0).abs() < 1e-6,
            "expected +Z, got {:?}",
            v.normal
        );
    }
}
