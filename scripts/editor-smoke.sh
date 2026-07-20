#!/usr/bin/env bash
# Rime — editor smoke test (M9.3): prove the editor is a client of a *live engine*, end to end.
#
# The rime-protocol crate's conformance tests prove the editor speaks the right bytes; they do NOT
# prove the two real processes talk. This does, exactly as ADR-0016 promises:
#
#   build rime-engine (C++) + editor (Rust) -> `editor --smoke` spawns
#   `rime-engine --editor-host <socket>` -> handshake -> pull schema + world snapshot ->
#   push an edit back -> Bye -> assert the engine exits cleanly
#
# GPU-free (the editor CHANNEL; the streamed viewport is a later brick), so it runs anywhere the
# engine builds — no window, no Vulkan. Mirrors scripts/sdk-smoke.sh's Conan discovery so it is
# self-sufficient. See docs/design/scene-format.md (the world it inspects) and the tools/editor,
# tools/rime-protocol, engine/editorhost READMEs.
set -euo pipefail

preset="dev"
while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   preset="${2:?--preset needs a value}"; shift 2 ;;
        --preset=*) preset="${1#*=}"; shift ;;
        -h|--help)  echo "Usage: scripts/editor-smoke.sh [--preset dev|release]"; exit 0 ;;
        *) echo "editor-smoke.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

case "$preset" in
    dev)     build_type="Debug";          cargo_flag="";          cargo_dir="debug" ;;
    release) build_type="RelWithDebInfo";  cargo_flag="--release"; cargo_dir="release" ;;
    *) echo "editor-smoke.sh: unknown preset '$preset' (expected dev or release)" >&2; exit 2 ;;
esac

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

build_dir="build/$preset"
toolchain="$repo_root/$build_dir/conan_toolchain.cmake"

# ── 1) Build the engine host (conan install first if the toolchain isn't there yet) ──────────────
if [ ! -f "$toolchain" ]; then
    if command -v conan >/dev/null 2>&1; then conan="conan"
    elif [ -x "$HOME/.rime-tools/bin/conan" ]; then conan="$HOME/.rime-tools/bin/conan"
    else echo "editor-smoke.sh: conan not found — run scripts/setup.sh first" >&2; exit 1
    fi
    say "conan install ($build_type)"
    # AV1 codecs built optimized even under Debug (see scripts/build.sh) — keeps the resolved codec
    # packages identical to the main build (no cache thrash) and dodges their debug-only asserts.
    "$conan" install . -of "$build_dir" -s build_type="$build_type" -s compiler.cppstd=20 \
        -s "libsvtav1/*:build_type=Release" -s "dav1d/*:build_type=Release" --build=missing
fi

say "build rime-engine ($preset)"
cmake --preset "$preset"
cmake --build --preset "$preset" --target rime_engine

# ── 2) Build the Rust editor ─────────────────────────────────────────────────────────────────────
say "build editor (cargo)"
( cd tools && cargo build -p editor $cargo_flag )

# ── 3) Run the smoke: editor spawns the engine host and drives a full session ────────────────────
engine_bin="$repo_root/$build_dir/bin/rime-engine"
editor_bin="$repo_root/tools/target/$cargo_dir/editor"
scene="$repo_root/samples/07-first-light/first_light.rscene"
[ -x "$engine_bin" ] || { echo "editor-smoke.sh: missing $engine_bin" >&2; exit 1; }
[ -x "$editor_bin" ] || { echo "editor-smoke.sh: missing $editor_bin" >&2; exit 1; }

say "run editor --smoke (editor channel: schema + snapshot + edit)"
"$editor_bin" --smoke --engine "$engine_bin" --scene "$scene"

# The streamed viewport: the engine renders a scene and streams it; the editor receives + LZ4-decodes
# real frames. Needs a Vulkan device on the engine side — lavapipe (mesa) in CI. On a host with no
# device the engine degrades to channel-only and this would time out, so it is gated on a device
# being discoverable (vulkaninfo). Locally, run it directly if you have any Vulkan ICD.
if command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo --summary >/dev/null 2>&1; then
    say "run editor --smoke --frames (streamed viewport: render → LZ4 → decode)"
    "$editor_bin" --smoke --frames 8 --engine "$engine_bin"

    # Same streamed-viewport path, but hosting a real --scene rather than the built-in demo (m9.5
    # passthrough): the engine loads first_light.rscene into the viewport world, renders it, and the
    # smoke picks a lit pixel + gizmo-edits an entity — proving serve_viewport honours --scene end to
    # end. (serve_viewport silently ignored --scene before this brick; without a test that path is a
    # CI blind spot.)
    say "run editor --smoke --frames --scene (streamed viewport hosts a loaded .rscene)"
    "$editor_bin" --smoke --frames 8 --engine "$engine_bin" --scene "$scene"
else
    say "skip viewport smoke — no Vulkan device (vulkaninfo not usable); channel smoke covered it"
fi

say "editor smoke: PASS"
