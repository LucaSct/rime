#!/usr/bin/env bash
# Open the authored city thermal demo with its known host and saved camera.
set -euo pipefail
courtyard_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$courtyard_root"
cmake --build --preset release --target thermal_courtyard -j 3
if [ "${1:-}" = "--editor" ]; then
    shift
    exec cargo run --manifest-path "$courtyard_root/tools/Cargo.toml" -p editor --features gui -- \
        --engine "$courtyard_root/build/release/bin/thermal-courtyard" \
        --scene "$courtyard_root/samples/thermal-courtyard/courtyard.rscene" "$@"
fi
exec "$courtyard_root/build/release/bin/thermal-courtyard" \
    --scene "$courtyard_root/samples/thermal-courtyard/courtyard.rscene" "$@"
