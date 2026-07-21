// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The cook cache (ADR-0024 §8): skip re-cooking a source whose bytes are unchanged since the last
//! cook with the same cooker version. The cache is *derived* data — a sidecar `cook-cache.txt` beside
//! the cooked files — keyed on **content**, not timestamps, so it is correct across a `git` checkout
//! that rewrites mtimes and byte-stable for CI. A hit reuses the already-written cooked files
//! untouched; any of {source bytes changed, cooker version bumped, a cooked file gone} forces a fresh
//! cook. This is the measure-before-optimize baseline for streaming: the multi-megabyte ICEM parts
//! (M6.6) re-cook only when they actually change.
//!
//! The record keys on the *primary source file's* bytes. That is exact for self-contained sources —
//! binary STL, `.glb`, PNG/JPEG, and embedded-buffer glTF — which is what M6 cooks. A `.gltf` that
//! references *external* `.bin`/image files is a known v1 limitation: edits to those siblings are not
//! seen, so bump `COOKER_VERSION` or clear the cache to force a re-cook. (Tracking referenced URIs is
//! an additive later improvement — the sidecar's columns need not change for it.)

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;

use crate::cooked::COOKER_VERSION;
use crate::manifest::ManifestEntry;

/// One source's cache record: the FNV-1a hash of its bytes, the cooker version that produced its
/// outputs, and the manifest entries those outputs correspond to (so a hit re-emits them without
/// re-reading the cooked files).
pub struct CacheRecord {
    pub src_hash: u64,
    pub cooker_version: u32,
    pub entries: Vec<ManifestEntry>,
}

impl CacheRecord {
    /// A cache hit: the source bytes still hash to `src_hash`, the cooker version matches this build,
    /// and every cooked file the record names still exists under `out_dir`. Any miss ⇒ re-cook (the
    /// safe direction — we never serve a file that isn't there or was cooked by a different cooker).
    pub fn is_fresh(&self, src_hash: u64, out_dir: &Path) -> bool {
        self.src_hash == src_hash
            && self.cooker_version == COOKER_VERSION
            && self
                .entries
                .iter()
                .all(|e| out_dir.join(&e.cooked_file).exists())
    }
}

/// Intern a cooked-kind string back to its `&'static str`, so a parsed cache line can rebuild a
/// `ManifestEntry`. Must know every kind `cook_*` emits (mesh/material/texture/skeleton/clip) — a kind
/// missing here silently drops that asset from a cache hit (the M6.7 skeleton/clip bug). An unknown
/// kind means a corrupt or foreign cache line — return `None` and drop it, which merely forces a
/// re-cook.
fn intern_kind(kind: &str) -> Option<&'static str> {
    match kind {
        "mesh" => Some("mesh"),
        "material" => Some("material"),
        "texture" => Some("texture"),
        "skeleton" => Some("skeleton"),
        "clip" => Some("clip"),
        "destructible" => Some("destructible"),
        "mesh_sdf" => Some("mesh_sdf"),
        _ => None,
    }
}

/// Parse a `cook-cache.txt` into a map keyed by the **primary source file** (the file whose bytes are
/// hashed — `cook_path` looks a source up by exactly this key). Each cooked asset carries its *own*
/// manifest source label separately (`<primary>` or `<primary>#material0`/`#skeleton`/…), so a hit
/// re-emits the exact manifest the full cook wrote. Malformed lines are skipped rather than erroring: a
/// cache we cannot fully trust simply yields fewer hits (⇒ more cooking), never wrong output. A missing
/// file is just an empty string ⇒ an empty map. (Older 6-column `v1` lines fail the column count and
/// are dropped, so a stale cache self-heals into a re-cook on the next run.)
pub fn parse(text: &str) -> BTreeMap<String, CacheRecord> {
    let mut map: BTreeMap<String, CacheRecord> = BTreeMap::new();
    for line in text.lines() {
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let cols: Vec<&str> = line.split('\t').collect();
        if cols.len() != 7 {
            continue;
        }
        // Columns: primary_source \t src_hash(hex) \t cooker_version \t entry_source \t kind \t
        //          id(hex) \t cooked_file. cols[0] groups (and is the cook-time lookup key); cols[3]
        //          is *this* asset's manifest source label, which may sub-qualify the primary with `#`.
        let (Ok(src_hash), Ok(cooker_version), Some(kind), Ok(id)) = (
            u64::from_str_radix(cols[1], 16),
            cols[2].parse::<u32>(),
            intern_kind(cols[4]),
            u64::from_str_radix(cols[5], 16),
        ) else {
            continue;
        };
        let primary = cols[0].to_string();
        let entry = ManifestEntry {
            source_path: cols[3].to_string(),
            kind,
            id,
            cooked_file: cols[6].to_string(),
        };
        match map.get_mut(&primary) {
            // A source's lines must agree on its hash and cooker version; a discordant line means the
            // cache is corrupt for that source, so we ignore it (the freshness check then re-cooks).
            Some(rec) if rec.src_hash == src_hash && rec.cooker_version == cooker_version => {
                rec.entries.push(entry);
            }
            Some(_) => {}
            None => {
                map.insert(
                    primary,
                    CacheRecord {
                        src_hash,
                        cooker_version,
                        entries: vec![entry],
                    },
                );
            }
        }
    }
    map
}

/// Render a **deterministic** `cook-cache.txt`: a banner (no timestamp), then one tab-separated line
/// per cooked asset — `primary_source \t src_hash \t cooker_version \t entry_source \t kind \t id \t
/// cooked_file` — sorted by primary source (the `BTreeMap`) then cooked file, so the same cook always
/// writes the same bytes. The `entry_source` column is each asset's own manifest label (crucially NOT
/// the grouping key), so a cache hit reconstructs the exact manifest — including `#material0`,
/// `#skeleton`, `#animation/<name>` sub-labels that share the primary's bytes.
pub fn render(records: &BTreeMap<String, CacheRecord>) -> String {
    let mut out = String::from("# rime-cook-cache v2\n");
    for (primary_source, rec) in records {
        let mut entries: Vec<&ManifestEntry> = rec.entries.iter().collect();
        entries.sort_by(|a, b| a.cooked_file.cmp(&b.cooked_file));
        for e in entries {
            // `{:016x}` mirrors the manifest's hex width, so both sidecars read the same way.
            let _ = writeln!(
                out,
                "{}\t{:016x}\t{}\t{}\t{}\t{:016x}\t{}",
                primary_source,
                rec.src_hash,
                rec.cooker_version,
                e.source_path,
                e.kind,
                e.id,
                e.cooked_file
            );
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(source_path: &str, kind: &'static str, cooked_file: &str, id: u64) -> ManifestEntry {
        ManifestEntry {
            source_path: source_path.into(),
            kind,
            id,
            cooked_file: cooked_file.into(),
        }
    }

    #[test]
    fn round_trips_through_render_and_parse() {
        // One primary source (`part.gltf`) with four cooked outputs whose manifest labels sub-qualify
        // the primary with `#`, spanning every asset kind including the M6.7 skeleton + clip. This is
        // the regression guard for the cache-hit manifest corruption: each entry must survive
        // render→parse with its OWN source label and kind intact — not collapsed to the grouping key,
        // and not dropped for being a skeleton/clip. The banner line is ignored on the way back in.
        let mut records = BTreeMap::new();
        records.insert(
            "part.gltf".to_string(),
            CacheRecord {
                src_hash: 0xdead_beef_0000_0001,
                cooker_version: COOKER_VERSION,
                entries: vec![
                    entry("part.gltf", "mesh", "part.rmesh", 0x11),
                    entry("part.gltf#material0", "material", "part.mat0.rmat", 0x22),
                    entry("part.gltf#skeleton", "skeleton", "part.rskel", 0x33),
                    entry("part.gltf#animation/Bend", "clip", "part.Bend.ranim", 0x44),
                ],
            },
        );
        let back = parse(&render(&records));
        assert_eq!(back.len(), 1);
        let rec = back.get("part.gltf").unwrap();
        assert_eq!(rec.src_hash, 0xdead_beef_0000_0001);
        assert_eq!(rec.cooker_version, COOKER_VERSION);
        assert_eq!(rec.entries.len(), 4);
        // Sub-qualified source labels and kinds are preserved exactly (the two bugs this guards).
        let mut got: Vec<(&str, &str)> = rec
            .entries
            .iter()
            .map(|e| (e.source_path.as_str(), e.kind))
            .collect();
        got.sort();
        assert_eq!(
            got,
            vec![
                ("part.gltf", "mesh"),
                ("part.gltf#animation/Bend", "clip"),
                ("part.gltf#material0", "material"),
                ("part.gltf#skeleton", "skeleton"),
            ]
        );
    }

    #[test]
    fn a_wrong_cooker_version_or_hash_is_not_fresh() {
        let rec = CacheRecord {
            src_hash: 7,
            cooker_version: COOKER_VERSION.wrapping_add(1),
            entries: vec![],
        };
        // Right hash, wrong cooker version → stale.
        assert!(!rec.is_fresh(7, Path::new("/nonexistent")));
        let rec2 = CacheRecord {
            src_hash: 7,
            cooker_version: COOKER_VERSION,
            entries: vec![],
        };
        // Wrong hash → stale even at the right version.
        assert!(!rec2.is_fresh(8, Path::new("/nonexistent")));
    }

    #[test]
    fn malformed_lines_are_dropped() {
        // A line with too few columns, a non-hex hash, and an unknown kind are each rejected — the
        // whole cache parses to empty, which just means "cook everything". (An older 6-column v1 line
        // lands in the first case, so a stale cache re-cooks rather than misreads.)
        let text = "# rime-cook-cache v2\n\
                    a.stl\t0000000000000001\t1\tmesh\t0000000000000000\ta.rmesh\n\
                    b.stl\tnothex\t1\tb.stl\tmesh\t0000000000000000\tb.rmesh\n\
                    c.stl\t0000000000000001\t1\tc.stl\tmystery\t0000000000000000\tc.rbin\n";
        assert!(parse(text).is_empty());
    }
}
