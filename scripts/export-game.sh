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
# A PowerShell twin is a follow-up, not a copy of this: the Windows dependency check needs dumpbin /
# the PE import table, and that is not a line-for-line translation of ldd.
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

# ── 2. The export record ──────────────────────────────────────────────────────────────────────
mapfile -t records < <(find "$build_dir/export" -name "$target.export" 2>/dev/null)
[ "${#records[@]}" -eq 1 ] ||
    die "expected one export record for '$target' under $build_dir/export, found ${#records[@]} (does its CMakeLists.txt call rime_game_content()?)"
exe="$(sed -n 's/^executable=//p' "${records[0]}")"
content="$(sed -n 's/^content=//p' "${records[0]}")"
[ -x "$exe" ] || die "the export record names $exe, which is not an executable"
[ -d "$content" ] || die "the export record names content $content, which is not a directory"

# ── 3. Shaders: embedded, so nothing loose may be needed ──────────────────────────────────────
loose_spv=$(find "$build_dir/bin" -maxdepth 2 -name '*.spv' 2>/dev/null | wc -l)
[ "$loose_spv" -eq 0 ] || die "$loose_spv loose .spv file(s) beside the binaries — a shader is being read from disk"

# ── 4. Copy ───────────────────────────────────────────────────────────────────────────────────
mkdir -p "$out_root"
bundle="$(cd "$out_root" && pwd)/$target"
case "$bundle/" in
    "$repo"/*) die "the bundle must be written outside the repository ($bundle)" ;;
esac
rm -rf "$bundle"
mkdir -p "$bundle"
cp "$exe" "$bundle/$target"
cp -R "$content" "$bundle/content"

cat >"$bundle/README.txt" <<EOF
$target — a game built with the Rime engine.

Run it from anywhere; it finds its content in the content/ directory beside it
(or wherever --content DIR says).

  ./$target                       play (the default): a window, keyboard input
  ./$target play --headless       play off-screen (no window)
  ./$target dedicated             a headless server: no window, no GPU, no Vulkan needed
  ./$target dedicated --ticks N --autopilot
                                  a bounded, deterministic run; prints a state digest
  ./$target browser | stream | host
                                  not implemented yet (exit code 3)
  ./$target --help                every option

Exit codes: 0 ok, 1 failed, 2 usage, 3 not yet implemented.
Shaders are compiled into the executable; there are no shader files.
Exported from $(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo unknown), preset $preset.
EOF

# ── 5. The dependency check ───────────────────────────────────────────────────────────────────
bin="$bundle/$target"
fail=0
if [ "$(uname -s)" = "Linux" ]; then
    needed=$(readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    say "NEEDED: $(echo "$needed" | tr '\n' ' ')"
    if echo "$needed" | grep -qi vulkan; then
        echo "  libvulkan is a LINK-TIME dependency — dedicated could not start without a Vulkan loader" >&2
        fail=1
    fi
    if readelf -d "$bin" | grep -qE '\((RPATH|RUNPATH)\)'; then
        echo "  the binary carries an RPATH/RUNPATH: $(readelf -d "$bin" | grep -E 'RPATH|RUNPATH')" >&2
        fail=1
    fi
    while read -r line; do
        case "$line" in
            *"not found"*) echo "  unresolved: $line" >&2; fail=1 ;;
        esac
        path=$(echo "$line" | sed -n 's/.*=> \(\/[^ ]*\).*/\1/p; t; s/^\(\/[^ ]*\).*/\1/p')
        [ -n "$path" ] || continue
        case "$path" in
            "$repo"/* | "$HOME"/* | *conan*) echo "  non-system library: $path" >&2; fail=1 ;;
            /lib/* | /lib64/* | /usr/lib/* | /usr/lib64/*) ;;
            *) echo "  library outside the system directories: $path" >&2; fail=1 ;;
        esac
    done < <(ldd "$bin")
    say "ldd: $(ldd "$bin" | wc -l) entries, all system libraries: $([ $fail -eq 0 ] && echo yes || echo NO)"
else
    say "dependency check: NOT RUN (Linux only for now; macOS wants otool -L)"
fi
[ "$fail" -eq 0 ] || die "the bundle depends on something a player's machine will not have"

say "bundle written: $bundle ($(du -sh "$bundle" | cut -f1); content: $(find "$bundle/content" -type f | wc -l) file(s))"
