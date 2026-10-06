#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
#
# Generate and cook the m19.8e perf world into <out> (default build/terrain-perf-world): sources
# from gen_splat_world.py's `perf` mode, cooked by `rime terrain-world` against the committed
# splat-world palette (its two terrain layers and material). Generated, never committed: ~25 MB.
#   samples/15-terrain/make_world.sh [out] [tiles-per-axis] [levels]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-$root/build/terrain-perf-world}"
tiles="${2:-32}"
levels="${3:-6}"
rime="${RIME:-$root/tools/target/release/rime}"
[ -x "$rime" ] || rime="$root/tools/target/debug/rime"
palette="$root/tests/assets/fixtures/splat_world/palette"
ids() { awk -F'\t' -v n="src/$1.terrainlayer.toml" '$1 == n {print $3}' "$palette/manifest.txt"; }
rm -rf "$out"
python3 "$root/tests/assets/fixtures/splat_world/gen_splat_world.py" perf "$(ids grass)" "$(ids rock)" \
    "$out/src" "$palette" "$tiles" "$levels"
"$rime" terrain-world "$out/src/terrain_perf.terrainworld.toml" --out "$out/world"
cp "$palette/manifest.txt" "$out/palette-manifest.txt"
