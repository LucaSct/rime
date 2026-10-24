#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
# Open an editable city, with an explicit full-runtime launch configuration.
set -euo pipefail
city_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$city_root"
cmake --build --preset release --target the_block the_block_host -j 3
# Cook fixtures produce runtime inputs; this does not run/waive the block performance gates.
ctest --test-dir build/release --output-on-failure -R '^block_demo_cook_'
city_scene="$city_root/build/release/city-editor.rscene"
if [ "${1:-}" = "--scene" ]; then
    city_scene="${2:?--scene needs a scene file}"
    shift 2
    # --scene opens an existing document; never generate over a misspelled path.
    [ -f "$city_scene" ] || { echo "city-editor: scene does not exist: $city_scene" >&2; exit 2; }
else
    if [ ! -e "$city_scene" ]; then
        "$city_root/build/release/bin/the_block" --write-city "$city_scene"
    fi
fi
exec cargo run --manifest-path "$city_root/tools/Cargo.toml" -p editor --features gui -- \
    --engine "$city_root/build/release/bin/the-block-host" --scene "$city_scene" \
    --assets "$city_root/build/release/samples/99-the-block/cooked/manifest.txt" \
    --game "$city_root/build/release/bin/the_block" --game-arg --play \
    --game-arg --cooked --game-arg "$city_root/build/release/samples/99-the-block/cooked" \
    --game-arg --viewmodel-content --game-arg "$city_root/build/release/samples/99-the-block/viewmodel-cooked" "$@"
