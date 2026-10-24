#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
# Open an editable copy of the bounded water/ice/fire/smoke city showcase.
set -euo pipefail
v2_root="$(cd "$(dirname "$0")/.." && pwd)"
v2_scene="$v2_root/build/release/city-v2.rscene"
mkdir -p "$v2_root/build/release"
if [ ! -e "$v2_scene" ]; then
    cp "$v2_root/samples/99-the-block/city-v2.rscene" "$v2_scene"
fi
exec "$v2_root/scripts/city-editor.sh" --scene "$v2_scene" "$@"
