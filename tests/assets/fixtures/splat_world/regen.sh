#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
#
# Regenerate the m19.8e end-to-end splat world (ADR-0073) — deliberately, never from a test.
# Sources come from gen_splat_world.py; EVERY cooked byte comes from the `rime` CLI:
#   rime cook            the material (material_quad.gltf's)          -> palette/
#   rime terrain-layer   two layers (grass, rock) over that material   -> palette/
#   rime terrain-world   4x4 tiles, 3 levels, bakes from palette/      -> world/
# and palette/manifest.txt gains the layer lines the engine's TerrainLayerBuilder resolves through.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
rime="${RIME:-$root/tools/target/debug/rime}"
cd "$here"
rm -rf src palette world
"$rime" cook ../material_quad.gltf --out palette > /dev/null
rm -f palette/cook-cache.txt palette/*.rmesh
sed -i "/\tmesh\t/d" palette/manifest.txt
mat="$(awk -F'\t' '$2 == "material" {print $3}' palette/manifest.txt)"
python3 gen_splat_world.py layers "$mat"
id_of() { sed -n 's/.*\.rtl (id \([0-9a-f]*\)).*/\1/p'; }
tex_of() { sed -n 's/.*_albedo_height\.rtex (id \([0-9a-f]*\)).*/\1/p'; }
for layer in grass rock; do
    out="$("$rime" terrain-layer "src/$layer.terrainlayer.toml" --out palette)"
    printf 'src/%s.terrainlayer.toml\tterrain_layer\t%s\t%s.rtl\n' "$layer" "$(id_of <<<"$out")" "$layer" >> palette/manifest.txt
    printf 'src/%s_albedo.png\ttexture\t%s\t%s_albedo_height.rtex\n' "$layer" "$(tex_of <<<"$out")" "$layer" >> palette/manifest.txt
    eval "${layer}_id=$(id_of <<<"$out")"
done
python3 gen_splat_world.py tiles "$grass_id" "$rock_id"
"$rime" terrain-world src/splat_world.terrainworld.toml --out world
