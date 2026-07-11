// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Integration test for the M6.6 content-hash cook cache (ADR-0024 §8): a re-cook of an unchanged
//! source is skipped without rewriting its cooked file, and a changed source re-cooks — both decided
//! by the source's *content hash*, never its mtime. Exercised through the STL path, the self-contained
//! format the cache is exact for.

use std::path::PathBuf;

use asset_pipeline::cook_path;
use asset_pipeline::texture::ColorSpace;

fn temp_out(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("rime_cache_{name}_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    dir
}

/// A minimal binary STL of a unit quad (two coplanar triangles sharing a diagonal). `shift` slides it
/// along +x so the *bytes* — and therefore the content hash — differ, without changing the topology.
fn quad_stl(shift: f32) -> Vec<u8> {
    let v = |x: f32, y: f32| [x + shift, y, 0.0f32];
    let tris = [
        ([0.0, 0.0, 1.0f32], [v(0.0, 0.0), v(1.0, 0.0), v(1.0, 1.0)]),
        ([0.0, 0.0, 1.0f32], [v(0.0, 0.0), v(1.0, 1.0), v(0.0, 1.0)]),
    ];
    let mut b = vec![0u8; 80]; // 80-byte header, all zero (deliberately not "solid")
    b.extend_from_slice(&(tris.len() as u32).to_le_bytes());
    for (n, vs) in &tris {
        for c in n {
            b.extend_from_slice(&c.to_le_bytes());
        }
        for vtx in vs {
            for c in vtx {
                b.extend_from_slice(&c.to_le_bytes());
            }
        }
        b.extend_from_slice(&0u16.to_le_bytes()); // per-triangle attribute word
    }
    b
}

#[test]
fn unchanged_source_hits_cache_changed_source_recooks() {
    let out = temp_out("skip");
    std::fs::create_dir_all(&out).unwrap();
    let src = out.join("part.stl");
    std::fs::write(&src, quad_stl(0.0)).unwrap();

    // First cook: a miss — nothing is cached yet.
    let r1 = cook_path(&src, &out, ColorSpace::Srgb).unwrap();
    assert_eq!((r1.sources_cooked, r1.sources_cached), (1, 0));

    // Poison the cooked file. If the next cook *skips*, this marker survives — an mtime-independent
    // proof the file was not rewritten (a rewrite would replace it with valid RMA1 bytes).
    let cooked = out.join("part.rmesh");
    let mut poisoned = std::fs::read(&cooked).unwrap();
    poisoned.extend_from_slice(b"POISON");
    std::fs::write(&cooked, &poisoned).unwrap();

    // Second cook, source bytes unchanged: a hit — the cooked file is left exactly as it was.
    let r2 = cook_path(&src, &out, ColorSpace::Srgb).unwrap();
    assert_eq!((r2.sources_cooked, r2.sources_cached), (0, 1));
    assert_eq!(
        std::fs::read(&cooked).unwrap(),
        poisoned,
        "a cache hit must not rewrite the cooked file"
    );

    // Change the source bytes: a different content hash → a miss → the file is re-cooked (marker gone,
    // valid RMA1 bytes back). This is what proves the key is content, not mtime.
    std::fs::write(&src, quad_stl(5.0)).unwrap();
    let r3 = cook_path(&src, &out, ColorSpace::Srgb).unwrap();
    assert_eq!((r3.sources_cooked, r3.sources_cached), (1, 0));
    assert_ne!(
        std::fs::read(&cooked).unwrap(),
        poisoned,
        "a changed source must re-cook"
    );
}

#[test]
fn a_deleted_cooked_file_forces_a_recook_even_when_the_source_is_unchanged() {
    // The cache must not claim a hit for a cooked file that no longer exists on disk (someone cleaned
    // the output dir but left cook-cache.txt): freshness requires the file to be present.
    let out = temp_out("missing");
    std::fs::create_dir_all(&out).unwrap();
    let src = out.join("part.stl");
    std::fs::write(&src, quad_stl(0.0)).unwrap();

    cook_path(&src, &out, ColorSpace::Srgb).unwrap();
    std::fs::remove_file(out.join("part.rmesh")).unwrap(); // cache still references it

    let r = cook_path(&src, &out, ColorSpace::Srgb).unwrap();
    assert_eq!((r.sources_cooked, r.sources_cached), (1, 0));
    assert!(
        out.join("part.rmesh").exists(),
        "the missing file was re-cooked"
    );
}
