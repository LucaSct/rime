#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
#
# export-game.sh — m20.2 (ADR-0076-m20.2): build a game and write its BUNDLE, the directory a
# player or a server operator receives (ADR-0046 §1).
#
#   scripts/export-game.sh <target> <out-dir> [--preset release|dev] [--no-build]
#
# writes
#
#   <out-dir>/<target>/<target>      the game executable (play, dedicated, … — one binary)
#   <out-dir>/<target>/content/      the game's content, found at run time beside the executable
#   <out-dir>/<target>/README.txt    the modes, in plain words
#
# Where a target's executable and content live is not guessed: `rime_game_content()` in CMake writes
# an export record (build/<preset>/export/<config>/<target>.export) and this script reads it.
#
# Shaders: there are none to copy. Every SPIR-V module the engine uses is compiled into the binary
# at build time (`rime_embed_shaders` in the top-level CMakeLists.txt). The script checks that claim
# rather than repeating it: it fails if the build tree holds a loose .spv the binary might want.
#
# Then the DEPENDENCY CHECK (Linux; ldd + readelf): every shared library the executable needs must
# resolve to a SYSTEM directory (/lib*, /usr/lib*), none to the repository, the build tree, $HOME or
# a Conan cache, and the binary must carry no RPATH/RUNPATH. libvulkan must NOT be a link-time
# dependency: the engine's loader (volk) dlopen()s it inside the device factory, which `dedicated`
# never enters — so a bundle runs on a box with no Vulkan at all. A violation fails the export.
#
# m20.3 adds rime-game.toml, authentic redistribution notices, measured ABI requirements and
# binary-hash-bound build provenance. Only the audited Linux hello_game profile is implemented;
# other targets need their own native packaging/dependency proof. Python 3.11+ is a BUILD tool,
# not a runtime dependency of the game.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
    echo "usage: scripts/export-game.sh <target> <out-dir> [--preset release|dev] [--no-build]" >&2
    exit 2
}

[ $# -ge 2 ] || usage
target="$1"; out_root="$2"; shift 2
preset="release"; build=1
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="${2:?--preset needs a value}"; shift 2 ;;
        --no-build) build=0; shift ;;
        *) usage ;;
    esac
done

say() { printf 'export-game: %s\n' "$*"; }
die() { printf 'export-game: FAILED — %s\n' "$*" >&2; exit 1; }
[ "$(uname -s)" = "Linux" ] || die "native packaging for this host is not implemented yet"
[ "$target" = "hello_game" ] || die "only the reviewed hello_game export is implemented"
case "$preset" in release|dev) ;; *) usage ;; esac
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else "export-game: Python 3.11+ required")'

# ── 1. Build ──────────────────────────────────────────────────────────────────────────────────
build_dir="$repo/build/$preset"
if [ "$build" -eq 1 ]; then
    if [ ! -f "$build_dir/CMakeCache.txt" ]; then
        say "no configured $preset build — running scripts/build.sh --preset $preset --no-tests --cpp-only"
        "$repo/scripts/build.sh" --preset "$preset" --no-tests --cpp-only
    fi
    # Judged by exit status, never by grepping the output (CLAUDE.md: a failed build leaves the
    # previous binary in place, and exporting THAT is exporting a stale game).
    say "building $target ($preset)"
    if ! (cd "$repo" && cmake --build --preset "$preset" --target "$target" >/dev/null); then
        die "cmake --build --preset $preset --target $target failed"
    fi
fi

# ── 2. Resolve the reviewed export record ─────────────────────────────────────────────────────
mapfile -t records < <(find "$build_dir/export" -name "$target.export" 2>/dev/null)
[ "${#records[@]}" -eq 1 ] ||
    die "expected one export record for '$target', found ${#records[@]} (reconfigure and build the game)"

# Shaders stay embedded. Keep this original guard in addition to the standalone execution proof.
loose_spv=$(find "$build_dir/bin" -maxdepth 2 -name '*.spv' 2>/dev/null | wc -l)
[ "$loose_spv" -eq 0 ] || die "$loose_spv loose .spv file(s) beside the binaries"

# Python handles staged copying, authentic notices, ELF requirements and strict manifest checks.
# It publishes only after verification, preserving an existing good bundle if any input is bad.
exec python3 "$repo/scripts/stage_game_bundle.py" "${records[0]}" "$out_root" --repository "$repo"
