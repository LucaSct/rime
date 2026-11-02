// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! glTF cooking, two behaviours found cooking the Venice assets: `--bc` must reach a glTF's own
//! images (it used to be dropped on the floor, so a glTF directory cooked uncompressed), and
//! byte-identical textures are written once per cook rather than once per glTF.
//!
//! The sources are copies of the committed `material_quad.gltf`, which names two external PNGs.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use asset_pipeline::cooked::{fnv1a_64, read_header};
use asset_pipeline::manifest::ManifestEntry;
use asset_pipeline::texture::{
    ColorSpace, TEXFMT_BC7_SRGB, TEXFMT_BC7_UNORM, TEXFMT_RGBA8_SRGB, TEXFMT_RGBA8_UNORM,
};
use asset_pipeline::{cook_path, CookOutput};

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/assets/fixtures")
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("rime_share_bc_{name}_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// Write `material_quad.gltf` as `<dir>/<stem>.gltf`. Its image URIs are made absolute so the PNGs
/// need not sit beside the copy -- they would otherwise be cooked as standalone textures too and
/// muddy what the test counts. `image` swaps in a different PNG for image 0 when given.
fn write_gltf_copy(dir: &Path, stem: &str, image0: Option<&str>) {
    let text = std::fs::read_to_string(fixtures().join("material_quad.gltf")).unwrap();
    // Forward slashes: a glTF URI is a JSON string, and a Windows path's backslashes are JSON
    // escapes ("invalid escape" on windows-latest). glTF URIs use `/`, which Windows accepts too.
    let abs = |png: &str| fixtures().join(png).to_string_lossy().replace('\\', "/");
    let text = text
        .replace(
            "\"uri\": \"checker.png\"",
            &format!("\"uri\": \"{}\"", abs(image0.unwrap_or("checker.png"))),
        )
        .replace(
            "\"uri\": \"normal.png\"",
            &format!("\"uri\": \"{}\"", abs("normal.png")),
        );
    std::fs::write(dir.join(format!("{stem}.gltf")), text).unwrap();
}

fn texture_entries(out: &CookOutput) -> Vec<&ManifestEntry> {
    out.manifest
        .iter()
        .filter(|e| e.kind == "texture")
        .collect()
}

/// The wire `format` of a cooked texture (3rd u32 of the payload) and the id its bytes hash to.
fn format_and_id(path: &Path) -> (u32, u64) {
    let bytes =
        std::fs::read(path).unwrap_or_else(|_| panic!("{} was not written", path.display()));
    let (_, payload) = read_header(&bytes).unwrap();
    (
        u32::from_le_bytes(payload[8..12].try_into().unwrap()),
        fnv1a_64(payload),
    )
}

fn rtex_files(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".rtex"))
        .collect();
    v.sort();
    v
}

#[test]
fn identical_textures_across_gltfs_are_written_once() {
    let src = temp_dir("dedupe_src");
    let out_dir = temp_dir("dedupe_out");
    write_gltf_copy(&src, "alpha", None);
    write_gltf_copy(&src, "beta", None);

    let out = cook_path(&src, &out_dir, ColorSpace::Srgb, false).unwrap();

    // material_quad cooks to three distinct textures; two copies must not make six files.
    let ids: BTreeSet<u64> = texture_entries(&out).iter().map(|e| e.id).collect();
    assert_eq!(ids.len(), 3);
    assert_eq!(
        rtex_files(&out_dir).len(),
        ids.len(),
        "one .rtex per distinct id, got {:?}",
        rtex_files(&out_dir)
    );

    // Both stems still list every texture, each resolving to an existing file that carries that id.
    for stem in ["alpha", "beta"] {
        let mine: Vec<_> = texture_entries(&out)
            .into_iter()
            .filter(|e| e.source_path.contains(&format!("{stem}.gltf#")))
            .collect();
        assert_eq!(mine.len(), 3, "{stem} lost a texture entry");
        for e in mine {
            let (_, id) = format_and_id(&out_dir.join(&e.cooked_file));
            assert_eq!(id, e.id, "{stem}: {} holds other bytes", e.cooked_file);
        }
    }

    // First occurrence (alpha sorts first) keeps today's names; the second writes nothing new.
    assert!(out_dir.join("alpha.img0.srgb.rtex").exists());
    assert!(!out_dir.join("beta.img0.srgb.rtex").exists());
    assert_eq!(out.textures_shared, 3);

    // The materials of both stems reference the same ids, which is what makes sharing safe.
    let manifest = std::fs::read_to_string(out_dir.join("manifest.txt")).unwrap();
    assert!(manifest.contains("beta.gltf#image0.srgb\ttexture"));
}

#[test]
fn sharing_survives_a_cache_hit_and_does_not_trust_a_changed_owner() {
    let src = temp_dir("stale_src");
    let out_dir = temp_dir("stale_out");
    write_gltf_copy(&src, "alpha", None);
    write_gltf_copy(&src, "beta", None);
    let first = cook_path(&src, &out_dir, ColorSpace::Srgb, false).unwrap();

    // Unchanged re-cook: everything from cache, same manifest, same sharing count.
    let again = cook_path(&src, &out_dir, ColorSpace::Srgb, false).unwrap();
    assert_eq!((again.sources_cooked, again.sources_cached), (0, 2));
    assert_eq!(again.textures_shared, first.textures_shared);
    // Compared sorted: a cache hit replays entries in cache-file order, and the manifest sorts them.
    let files = |o: &CookOutput| -> Vec<(String, u64, String)> {
        let mut v: Vec<_> = o
            .manifest
            .iter()
            .map(|e| (e.source_path.clone(), e.id, e.cooked_file.clone()))
            .collect();
        v.sort();
        v
    };
    assert_eq!(files(&again), files(&first));

    // alpha OWNS the shared files; give it a different base-colour image. beta's cached entries
    // point at alpha's file, which now holds other bytes, so beta must not be served from cache.
    write_gltf_copy(&src, "alpha", Some("terrain_layer_albedo.png"));
    let changed = cook_path(&src, &out_dir, ColorSpace::Srgb, false).unwrap();
    assert_eq!(changed.sources_cooked, 2, "beta's pointers were stale");
    for e in texture_entries(&changed) {
        let (_, id) = format_and_id(&out_dir.join(&e.cooked_file));
        assert_eq!(
            id, e.id,
            "{} -> {} holds other bytes",
            e.source_path, e.cooked_file
        );
    }
}

#[test]
fn bc_reaches_gltf_images_and_keeps_each_maps_color_space() {
    let raw_dir = temp_dir("bc_raw");
    let bc_dir = temp_dir("bc_bc");
    let src = fixtures().join("material_quad.gltf");
    let raw = cook_path(&src, &raw_dir, ColorSpace::Srgb, false).unwrap();
    let bc = cook_path(&src, &bc_dir, ColorSpace::Srgb, true).unwrap();

    let by_label = |o: &CookOutput, dir: &Path| -> BTreeMap<String, u32> {
        texture_entries(o)
            .iter()
            .map(|e| {
                (
                    e.source_path.clone(),
                    format_and_id(&dir.join(&e.cooked_file)).0,
                )
            })
            .collect()
    };
    let raw_formats = by_label(&raw, &raw_dir);
    let bc_formats = by_label(&bc, &bc_dir);
    assert_eq!(raw_formats.len(), 3);
    assert_eq!(
        raw_formats.keys().collect::<Vec<_>>(),
        bc_formats.keys().collect::<Vec<_>>()
    );

    for (label, raw_fmt) in &raw_formats {
        // Usage decides the colour space, so --bc must land on the matching BC7 variant.
        let expected = match *raw_fmt {
            TEXFMT_RGBA8_SRGB => TEXFMT_BC7_SRGB,
            TEXFMT_RGBA8_UNORM => TEXFMT_BC7_UNORM,
            other => panic!("{label}: unexpected uncompressed format {other}"),
        };
        assert_eq!(bc_formats[label], expected, "{label}");
    }
}

#[test]
fn toggling_bc_into_the_same_directory_recooks_the_gltf() {
    let src = temp_dir("toggle_src");
    let out_dir = temp_dir("toggle_out");
    write_gltf_copy(&src, "quad", None);
    let is_bc = |f: u32| f == TEXFMT_BC7_SRGB || f == TEXFMT_BC7_UNORM;

    let with_bc = cook_path(&src, &out_dir, ColorSpace::Srgb, true).unwrap();
    assert_eq!(with_bc.sources_cooked, 1);
    for e in texture_entries(&with_bc) {
        assert!(is_bc(format_and_id(&out_dir.join(&e.cooked_file)).0));
    }

    let without = cook_path(&src, &out_dir, ColorSpace::Srgb, false).unwrap();
    assert_eq!(
        (without.sources_cooked, without.sources_cached),
        (1, 0),
        "a flag flip must not be served from the cache"
    );
    assert_eq!(texture_entries(&without).len(), 3);
    for e in texture_entries(&without) {
        let (fmt, id) = format_and_id(&out_dir.join(&e.cooked_file));
        assert!(!is_bc(fmt), "{} is still BC7", e.cooked_file);
        assert_eq!(id, e.id);
    }
}
