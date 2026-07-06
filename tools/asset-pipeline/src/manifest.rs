// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The cook manifest writer. The manifest is derived data — regenerable from the sources, so it can
//! never lie — mapping each source asset to its kind, content-hash id, and cooked filename. The C++
//! `engine/assets` `Manifest::parse` reads exactly this format (ADR-0024).

use std::fmt::Write as _;

/// One asset's manifest line, before rendering.
pub struct ManifestEntry {
    pub source_path: String,
    pub kind: &'static str,
    pub id: u64,
    pub cooked_file: String,
}

/// Render a **deterministic** manifest: a banner (no timestamp), then one tab-separated line per
/// asset, sorted by source path so no map/iteration order can leak into the bytes.
pub fn render(entries: &[ManifestEntry]) -> String {
    let mut sorted: Vec<&ManifestEntry> = entries.iter().collect();
    sorted.sort_by(|a, b| a.source_path.cmp(&b.source_path));

    let mut out = String::from("# rime-manifest v1\n");
    for e in sorted {
        // `{:016x}` — 16 lowercase hex digits, exactly what the C++ manifest reader parses.
        let _ = writeln!(
            out,
            "{}\t{}\t{:016x}\t{}",
            e.source_path, e.kind, e.id, e.cooked_file
        );
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_sorted_tab_separated_lines_with_a_banner() {
        let entries = vec![
            ManifestEntry {
                source_path: "b.gltf".into(),
                kind: "mesh",
                id: 0xff,
                cooked_file: "b.rmesh".into(),
            },
            ManifestEntry {
                source_path: "a.gltf".into(),
                kind: "mesh",
                id: 0x0123_4567_89ab_cdef,
                cooked_file: "a.rmesh".into(),
            },
        ];
        let text = render(&entries);
        assert_eq!(
            text,
            "# rime-manifest v1\n\
             a.gltf\tmesh\t0123456789abcdef\ta.rmesh\n\
             b.gltf\tmesh\t00000000000000ff\tb.rmesh\n"
        );
    }
}
