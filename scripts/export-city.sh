#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
set -euo pipefail
city_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$city_root"
[ $# -ge 1 ] || { echo 'usage: scripts/export-city.sh NEW-OUTPUT-DIRECTORY [--scene file.rscene]' >&2; exit 2; }
city_destination="$1"; shift
cmake --build --preset release --target the_block -j 3
# Cook fixtures produce runtime inputs; this does not run/waive the block performance gates.
ctest --test-dir build/release --output-on-failure -R '^block_demo_cook_'
city_scene="$city_root/build/release/city-editor.rscene"
if [ "${1:-}" = "--scene" ]; then
    city_scene="${2:?--scene needs a file}"
    shift 2
elif [ ! -e "$city_scene" ]; then
    "$city_root/build/release/bin/the_block" --write-city "$city_scene"
fi
[ $# -eq 0 ] || { echo 'city export: unknown arguments' >&2; exit 2; }
exec python3 "$city_root/scripts/export_city_dev.py" "$city_destination" --scene "$city_scene"
