// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Integration tests for the M6.2 glTF mesh cooker, exercised against the shared fixtures in
//! `tests/assets/fixtures/`. The committed `quad.rmesh` is the file the C++ `engine/assets` test
//! reads, so the `quad_cooks_to_the_committed_fixture_bytes` case is the cross-language drift alarm:
//! if the cooked format changes, this fails until the fixture is regenerated (`rime cook
//! tests/assets/fixtures/quad.gltf --out tests/assets/fixtures`).

use std::path::{Path, PathBuf};

use asset_pipeline::cooked::read_header;
use asset_pipeline::gltf_import;
use asset_pipeline::material::AlphaMode;
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

// ── M6.4: glTF material import ────────────────────────────────────────────────────────────────────
// material_quad.gltf is a unit quad with one material whose factors are the distinct values the
// cross-language material round-trip uses, referencing checker.png (base color + emissive) and
// normal.png. It exercises the whole material path: factor mapping, per-usage colour space, texture
// dedup, and the normal-map → tangents gate.

// A fresh, per-test output directory under the system temp dir (no external tempfile crate).
fn temp_out(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("rime_cook_{name}_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    dir
}

fn stride_of(rmesh: &[u8]) -> u32 {
    let (_, payload) = read_header(rmesh).unwrap();
    u32::from_le_bytes(payload[4..8].try_into().unwrap()) // header: [attribs, stride, ...]
}

#[test]
fn material_import_maps_the_gltf_factors() {
    let import =
        asset_pipeline::import_gltf_materials(&fixtures().join("material_quad.gltf")).unwrap();
    assert_eq!(import.materials.len(), 1);
    let m = &import.materials[0];
    let mat = &m.material;
    assert_eq!(mat.base_color, [0.8, 0.4, 0.2, 1.0]);
    assert_eq!(mat.emissive, [0.1, 0.2, 0.3]);
    assert_eq!(mat.metallic, 0.25);
    assert_eq!(mat.roughness, 0.6);
    assert_eq!(mat.normal_scale, 0.5); // from normalTexture.scale
    assert_eq!(mat.occlusion_strength, 0.75); // from occlusionTexture.strength
    assert_eq!(mat.alpha_cutoff, 0.3);
    assert_eq!(mat.alpha_mode, AlphaMode::Mask);
    assert!(m.has_normal_map, "the material has a normalTexture");
}

#[test]
fn material_textures_dedup_by_image_and_color_space() {
    let import =
        asset_pipeline::import_gltf_materials(&fixtures().join("material_quad.gltf")).unwrap();
    let m = &import.materials[0].material;
    // checker.png used as base color AND emissive — both sRGB colour — is one cooked asset.
    assert_eq!(m.base_color_tex, m.emissive_tex);
    // checker.png used as metallic-roughness AND occlusion — both linear data — is one cooked asset
    // (the glTF ORM-packing case).
    assert_eq!(m.metallic_roughness_tex, m.occlusion_tex);
    // The SAME image cooked in two colour spaces is two distinct assets (different bytes → different id).
    assert_ne!(m.base_color_tex, m.metallic_roughness_tex);
    // normal.png is present, linear, and distinct from the checker cooks.
    assert_ne!(m.normal_tex, 0);
    assert_ne!(m.normal_tex, m.base_color_tex);
    // Two source images, one used in two colour spaces → exactly three unique cooked textures.
    assert_eq!(import.textures.len(), 3);
}

#[test]
fn a_normal_mapped_gltf_cooks_tangents_and_a_plain_one_does_not() {
    // material_quad's material has a normal map → the mesh cooks with tangents (48-byte stride).
    let out = temp_out("tangent_gate");
    asset_pipeline::cook_path(
        &fixtures().join("material_quad.gltf"),
        &out,
        ColorSpace::Srgb,
    )
    .unwrap();
    assert_eq!(
        stride_of(&std::fs::read(out.join("material_quad.rmesh")).unwrap()),
        48,
        "a normal-mapped mesh must carry tangents"
    );

    // quad.gltf names no material → no normal map → the mesh stays plain P/N/UV (32-byte stride).
    let plain = temp_out("tangent_gate_plain");
    asset_pipeline::cook_path(&fixtures().join("quad.gltf"), &plain, ColorSpace::Srgb).unwrap();
    assert_eq!(
        stride_of(&std::fs::read(plain.join("quad.rmesh")).unwrap()),
        32,
        "a mesh with no normal map must not pay for tangents"
    );
}

#[test]
fn cook_gltf_writes_every_asset_deterministically() {
    let a = temp_out("mat_files_a");
    let b = temp_out("mat_files_b");
    asset_pipeline::cook_path(&fixtures().join("material_quad.gltf"), &a, ColorSpace::Srgb)
        .unwrap();
    asset_pipeline::cook_path(&fixtures().join("material_quad.gltf"), &b, ColorSpace::Srgb)
        .unwrap();

    // The cooked set: the mesh, one material, three unique textures, and the manifest — every file
    // byte-identical across two independent cooks (no map/iteration order leaks into the output).
    for f in [
        "material_quad.rmesh",
        "material_quad.mat0.rmat",
        "material_quad.img0.srgb.rtex", // checker as base color / emissive
        "material_quad.img0.lin.rtex",  // checker as metallic-roughness / occlusion
        "material_quad.img1.lin.rtex",  // normal map
        "manifest.txt",
    ] {
        let bytes_a = std::fs::read(a.join(f)).unwrap_or_else(|_| panic!("cook did not write {f}"));
        let bytes_b = std::fs::read(b.join(f)).unwrap();
        assert_eq!(bytes_a, bytes_b, "{f} must cook deterministically");
    }
}

#[test]
fn skinned_skeleton_and_clip_cook_to_the_committed_fixture_bytes() {
    // The M6.7 cross-language drift alarm: the committed skinned.rskel / skinned.Spin.ranim are what
    // the C++ fixture_test reads, so if the cooked skeleton or clip format changes, this fails until
    // the fixtures are regenerated (`rime cook tests/assets/fixtures/skinned.gltf --out
    // tests/assets/fixtures`).
    let out = temp_out("skinned");
    asset_pipeline::cook_path(&fixtures().join("skinned.gltf"), &out, ColorSpace::Srgb).unwrap();
    for name in ["skinned.rskel", "skinned.Spin.ranim"] {
        let cooked = std::fs::read(out.join(name)).unwrap();
        let committed = std::fs::read(fixtures().join(name)).unwrap();
        assert_eq!(
            cooked, committed,
            "{name} diverged from the committed fixture — regenerate it deliberately"
        );
    }
}

#[test]
fn skinned_import_reorders_joints_parent_first_and_maps_clip_targets() {
    use asset_pipeline::clip::ChannelPath;

    // skinned.gltf lists its skin joints CHILD-FIRST; import must reorder them into topological order.
    let imp = asset_pipeline::import_gltf_skeleton(&fixtures().join("skinned.gltf"))
        .unwrap()
        .expect("skinned.gltf has a skin");
    assert_eq!(imp.skeleton.joints.len(), 2);
    assert_eq!(imp.skeleton.joints[0].parent, -1); // root A first
    assert_eq!(imp.skeleton.joints[1].parent, 0); // child B, parent A
    assert_eq!(imp.skeleton.joints[0].translation, [0.0, 0.0, 0.0]); // A at the origin
    assert_eq!(imp.skeleton.joints[1].translation, [2.0, 0.0, 0.0]); // B two units along +X

    // The clip's channels resolve to the reordered joint indices: A (joint 0) slides, B (joint 1) spins.
    let clips = asset_pipeline::import_gltf_clips(&fixtures().join("skinned.gltf")).unwrap();
    assert_eq!(clips.len(), 1);
    assert_eq!(clips[0].name, "Spin");
    assert_eq!(clips[0].joint_count, 2);
    assert_eq!(clips[0].duration, 1.0);
    assert!(clips[0]
        .channels
        .iter()
        .any(|c| c.target_joint == 0 && c.path == ChannelPath::Translation));
    assert!(clips[0]
        .channels
        .iter()
        .any(|c| c.target_joint == 1 && c.path == ChannelPath::Rotation));
}

#[test]
fn wall_fractures_to_the_committed_fixture_bytes() {
    // The M8.1 cross-language drift alarm (mirrors the mesh/skeleton ones above): the committed
    // wall.rdest is what the C++ assets test reads — and registers, part by part, into a real
    // PhysicsWorld. If the fracture cook's format or Voronoi partition ever changes, this fails until
    // the fixture is regenerated deliberately (`rime fracture --size 2 1.5 0.3 --parts 16 --seed
    // 12648430 --out tests/assets/fixtures --name wall`; 12648430 == 0xC0FFEE).
    use asset_pipeline::fracture::{fracture_box, FractureConfig};
    let committed = std::fs::read(fixtures().join("wall.rdest")).unwrap();
    let cooked = fracture_box(&FractureConfig::wall([1.0, 0.75, 0.15], 16, 0xC0FFEE))
        .unwrap()
        .cook()
        .0;
    assert_eq!(
        cooked, committed,
        "fracture cook diverged from the committed fixture — regenerate it deliberately"
    );
}

#[test]
fn cube_sdf_cooks_to_the_committed_fixture_bytes() {
    // The M10.4a cross-language drift alarm (mirrors the mesh/skeleton/destructible ones above):
    // the committed cube.rsdf is what the C++ assets test reads and samples. cube.stl is the SAME
    // hand-authored, hard-edge-shaded cube the M6.6 STL fixture already uses (its README explains
    // the corner/winding convention) — reusing it here means the SDF fixture needs no new source
    // geometry. If the SDF cook's format, resolution policy, or algorithm ever changes, this fails
    // until the fixture is regenerated deliberately (`rime sdf tests/assets/fixtures/cube.stl --out
    // tests/assets/fixtures --name cube`).
    use asset_pipeline::sdf::{compute_sdf, SdfCookConfig};
    use asset_pipeline::stl::import_stl_binary;

    let committed = std::fs::read(fixtures().join("cube.rsdf")).unwrap();
    let bytes = std::fs::read(fixtures().join("cube.stl")).unwrap();
    let mesh = import_stl_binary(&bytes).unwrap().mesh;
    let vertices: Vec<[f32; 3]> = mesh.vertices.iter().map(|v| v.position).collect();
    let triangles: Vec<[u32; 3]> = mesh
        .indices
        .chunks_exact(3)
        .map(|c| [c[0], c[1], c[2]])
        .collect();
    let cooked = compute_sdf(&vertices, &triangles, &SdfCookConfig::for_mesh())
        .unwrap()
        .volume
        .cook()
        .0;
    assert_eq!(
        cooked, committed,
        "SDF cook diverged from the committed fixture — regenerate it deliberately"
    );
}

#[test]
fn cube_sdf_cook_is_byte_stable() {
    use asset_pipeline::sdf::{compute_sdf, SdfCookConfig};
    use asset_pipeline::stl::import_stl_binary;

    let bytes = std::fs::read(fixtures().join("cube.stl")).unwrap();
    let mesh = import_stl_binary(&bytes).unwrap().mesh;
    let vertices: Vec<[f32; 3]> = mesh.vertices.iter().map(|v| v.position).collect();
    let triangles: Vec<[u32; 3]> = mesh
        .indices
        .chunks_exact(3)
        .map(|c| [c[0], c[1], c[2]])
        .collect();
    let a = compute_sdf(&vertices, &triangles, &SdfCookConfig::for_mesh())
        .unwrap()
        .volume
        .cook();
    let b = compute_sdf(&vertices, &triangles, &SdfCookConfig::for_mesh())
        .unwrap()
        .volume
        .cook();
    assert_eq!(
        a.0, b.0,
        "cooking the same source twice must be byte-identical"
    );
    assert_eq!(a.1, b.1);
}
