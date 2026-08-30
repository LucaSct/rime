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
    # libsvtav1/4.2.0 is our own recipe; it must be in the cache before install resolves it.
    "$(dirname "$0")/conan-export-local.sh" "$conan"
    say "conan install ($build_type)"
    # AV1 codecs built optimized even under Debug (see scripts/build.sh) — keeps the resolved codec
    # packages identical to the main build (no cache thrash) and dodges their debug-only asserts.
    "$conan" install . -of "$build_dir" -s build_type="$build_type" -s compiler.cppstd=20 \
        -s "libsvtav1/*:build_type=Release" -s "dav1d/*:build_type=Release" --build=missing
fi

say "build rime-engine + rime-blockgen ($preset)"
cmake --preset "$preset"
# rime_blockgen too (m14.1): the block scene is GENERATED, not checked in, and opening it is the
# case this smoke exists for. See the block section below.
cmake --build --preset "$preset" --target rime_engine rime_blockgen the_block_host

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

# ── 4) The block: the scene the engine actually SHIPS (m14.1, ADR-0037) ──────────────────────────
#
# THIS IS THE CASE THIS SMOKE WAS MISSING. Until m14.1 the editor could not open the block at all —
# `rime-blockgen` writes `blockkit::SlabRole`, the host had never registered it, and the scene load
# failed on the newest content in the repo. This job stayed green through all seven PRs of the M13
# stack, because it only ever opened a synthetic four-entity scene.
#
# Generated rather than checked in, deliberately: a fixture committed today is a fixture that stops
# tracking the generator tomorrow, and "the editor opens what blockgen writes" is the actual claim.
say "generate the block scene (rime-blockgen)"
blockgen_bin="$repo_root/$build_dir/bin/rime-blockgen"
block_scene="$repo_root/$build_dir/editor-smoke-block.rscene"
[ -x "$blockgen_bin" ] || { echo "editor-smoke.sh: missing $blockgen_bin" >&2; exit 1; }
"$blockgen_bin" --out "$block_scene" --stats

# ── 5) TWO HOSTS, ONE SCENE (m15.2) ─────────────────────────────────────────────────────────────
#
# The engine's host registers the ENGINE's components and nothing else; the demo builds its own host
# that adds `blockkit` on top. Same scene, same unmodified editor, `--engine` pointed at a different
# binary — which is the whole of ADR-0038 §2: a game gets its components into the inspector by
# building one CMake target, not by editing engine C++.
#
# The assertion is the DIFFERENCE. The engine host must degrade (loading the scene, dropping the
# types it does not know, and SAYING how many); the game's host must be clean. If they ever report
# the same thing, either the engine host has grown a game's module back or the game's host is not
# registering anything — and both look fine from any other angle.
say "run editor --smoke --scene block.rscene through the ENGINE's host (expect a degraded load)"
engine_out="$("$editor_bin" --smoke --engine "$engine_bin" --scene "$block_scene" 2>&1)"
echo "$engine_out" | grep -E "skipped|editor <-> engine OK" || true
echo "$engine_out" | grep -q "component(s) skipped" || {
    echo "editor-smoke.sh: the engine host opened a game's scene with nothing skipped — it should" \
         "not know what a SlabRole is" >&2; exit 1; }

say "run editor --smoke --scene block.rscene through the GAME's host (expect 0 skipped)"
blockhost_bin="$repo_root/$build_dir/bin/the-block-host"
[ -x "$blockhost_bin" ] || { echo "editor-smoke.sh: missing $blockhost_bin" >&2; exit 1; }
game_out="$("$editor_bin" --smoke --engine "$blockhost_bin" --scene "$block_scene" 2>&1)"
echo "$game_out" | grep -E "0 skipped|editor <-> engine OK" || true
echo "$game_out" | grep -q "component(s) skipped" && {
    echo "editor-smoke.sh: the game's own host skipped components of its own scene" >&2; exit 1; }
echo "$game_out" | grep -q ", 0 skipped" || {
    echo "editor-smoke.sh: the game's host did not report a clean load" >&2; exit 1; }

# The streamed viewport: the engine renders a scene and streams it; the editor receives + LZ4-decodes
# real frames. Needs a Vulkan device on the engine side — lavapipe (mesa) in CI. On a host with no
# device the engine degrades to channel-only and this would time out, so it is gated on a device
# being discoverable (vulkaninfo). Locally, run it directly if you have any Vulkan ICD.
if command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo --summary >/dev/null 2>&1; then
    say "run editor --smoke --frames (streamed viewport: render → LZ4 → decode)"
    "$editor_bin" --smoke --frames 8 --engine "$engine_bin" --min-coverage 10

    # Same streamed-viewport path, but hosting a real --scene rather than the built-in demo (m9.5
    # passthrough): the engine loads first_light.rscene into the viewport world, renders it, and the
    # smoke picks a lit pixel + gizmo-edits an entity — proving serve_viewport honours --scene end to
    # end. (serve_viewport silently ignored --scene before this brick; without a test that path is a
    # CI blind spot.)
    say "run editor --smoke --frames --scene (streamed viewport hosts a loaded .rscene)"
    "$editor_bin" --smoke --frames 8 --engine "$engine_bin" --scene "$scene" --min-coverage 10

    # THE BLOCK, through the streamed viewport, via the GAME's own host (m15.4). This run was
    # refused for two bricks with a comment saying why: the block's slabs draw as cooked fracture
    # parts, the viewport built none, and adding the run anyway would have asserted "the frame is
    # black" and called it coverage. m15.4 closed that by giving a game somewhere to say what its
    # components LOOK like — `run_editor_host`'s ScenePreparer, which `the-block-host` answers with
    # blockkit's palette plus one preview box per cooked pattern.
    #
    # COVERAGE IS THE ASSERTION, not "a frame arrived". The block fills most of the frame, so a
    # floor requiring the picture to be substantially drawn is what separates "the buildings are
    # there" from "one lamp is lit and the rest resolved to nothing" — the exact failure the old
    # refusal existed to avoid asserting past.
    say "run editor --smoke --frames --scene block.rscene through the GAME's host (m15.4)"
    "$editor_bin" --smoke --frames 8 --engine "$blockhost_bin" --scene "$block_scene" \
        --min-coverage 40

    # THE ENGINE'S OWN ROUTE, with no game host and no game code: a scene that names a cooked mesh
    # by CONTENT ID resolves through the m15.1 asset bridge and draws. Authored the way a user would
    # — the editor places the mesh and saves the scene — rather than from a fixture with hand-written
    # reflection type hashes in it, which would rot the first time a component changed shape.
    zoo_manifest="$repo_root/$build_dir/samples/08-gltf-zoo/cooked/manifest.txt"
    if [ -f "$zoo_manifest" ]; then
        mesh_id="$(awk -F'\t' '$2=="mesh"{print $3; exit}' "$zoo_manifest")"
        placed="$repo_root/$build_dir/editor-smoke-placed.rscene"
        say "author a scene that names a cooked mesh by content id (place + save)"
        "$editor_bin" --smoke --engine "$engine_bin" --assets "$zoo_manifest" \
            --place-mesh "$mesh_id" --place-at 0,0.6,2.2 --save "$placed" >/dev/null
        grep -q "rime::render::MeshAsset" "$placed" || {
            echo "editor-smoke.sh: the saved scene does not name a mesh by asset id" >&2; exit 1; }

        # THE ASSERTION IS THE DIFFERENCE, like the two hosts above. The same scene, the same engine,
        # the same binary — with the manifest the bridge resolves the reference and the mesh draws;
        # without it there is nothing to resolve the id against and the entity stays present,
        # editable and undrawn. If both runs said the same thing the bridge would not be doing the
        # work, and a coverage number alone cannot see that (the cube lands in front of the floor,
        # so it replaces lit pixels rather than adding them — measured, not assumed).
        say "reopen it in the viewport WITH --assets (expect the bridge to resolve it)"
        with_out="$("$editor_bin" --smoke --frames 8 --engine "$engine_bin" --scene "$placed" \
            --assets "$zoo_manifest" 2>&1)"
        echo "$with_out" | grep -E "resolved [0-9]+ mesh reference|viewport OK" || true
        echo "$with_out" | grep -qE "resolved [1-9][0-9]* mesh reference" || {
            echo "editor-smoke.sh: --assets did not resolve the scene's MeshAsset" >&2; exit 1; }

        say "reopen it WITHOUT --assets (expect no resolve — the id has nothing to resolve against)"
        without_out="$("$editor_bin" --smoke --frames 8 --engine "$engine_bin" --scene "$placed" 2>&1)"
        echo "$without_out" | grep -q "mesh reference" && {
            echo "editor-smoke.sh: the viewport resolved meshes with no manifest — the run with" \
                 "--assets proves nothing" >&2; exit 1; }
        echo "$without_out" | grep -E "viewport OK" || true
    else
        say "skip the cooked-mesh viewport proof — 08-gltf-zoo has not been cooked in this build"
    fi
else
    say "skip viewport smoke — no Vulkan device (vulkaninfo not usable); channel smoke covered it"
fi

say "editor smoke: PASS"
