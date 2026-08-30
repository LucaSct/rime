#!/usr/bin/env bash
# Rime — the authoring round trip (m14.4): Milestone 14's "done when".
#
#   open the shipped block in the editor, change it, save it, and run the changed scene in the game
#
# Four processes and one file, in order:
#
#   rime-blockgen        writes block.rscene          (the content the engine ships)
#   the_block --scene    runs it, prints a digest     (the game, before)
#   editor --smoke       opens it, edits it, saves it (the editor, through the live engine host)
#   the_block --scene    runs the SAVED file          (the game, after)
#
# THE DIGESTS MUST DIFFER, and that assertion is the whole point. "The game ran the file the editor
# saved" is worth very little alone — it is equally true of a game that quietly regenerated its own
# level and ignored the file. Two runs whose placement digests differ are provably running different
# scenes, which is the claim M14 actually makes. And both runs must still pass every one of the
# demo's own claims, so the edit produced a scene that WORKS rather than merely a different one.
#
# Needs both toolchains and the cooked `.rdest` set (the demo's CTest fixtures produce it). It runs
# from the editor-smoke CI job, which already has Rust, Conan and a Vulkan runtime.
set -euo pipefail

preset="dev"
while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   preset="${2:?--preset needs a value}"; shift 2 ;;
        --preset=*) preset="${1#*=}"; shift ;;
        -h|--help)  echo "Usage: scripts/authoring-round-trip.sh [--preset dev|release]"; exit 0 ;;
        *) echo "authoring-round-trip.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

case "$preset" in
    dev)     cargo_flag="";          cargo_dir="debug" ;;
    release) cargo_flag="--release"; cargo_dir="release" ;;
    *) echo "authoring-round-trip.sh: unknown preset '$preset'" >&2; exit 2 ;;
esac

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

build_dir="build/$preset"
work="$build_dir/round-trip"
mkdir -p "$work"

say "build the engine host, the generator, the demo, and the editor"
cmake --preset "$preset"
cmake --build --preset "$preset" --target rime_engine rime_blockgen the_block
( cd tools && cargo build -p editor $cargo_flag )

engine_bin="$repo_root/$build_dir/bin/rime-engine"
blockgen_bin="$repo_root/$build_dir/bin/rime-blockgen"
demo_bin="$repo_root/$build_dir/bin/the_block"
editor_bin="$repo_root/tools/target/$cargo_dir/editor"
for b in "$engine_bin" "$blockgen_bin" "$demo_bin" "$editor_bin"; do
    [ -x "$b" ] || { echo "authoring-round-trip.sh: missing $b" >&2; exit 1; }
done

# The demo consumes nine cooked `.rdest` files. They are produced by CTest fixtures rather than
# checked in (a committed fixture stops tracking its generator), so drive those first.
say "cook the block's destructibles"
( cd "$build_dir" && ctest -R block_demo_cook --output-on-failure >/dev/null )

original="$work/block.rscene"
edited="$work/block-edited.rscene"
rm -f "$original" "$edited"

say "1/4  rime-blockgen writes the block"
"$blockgen_bin" --out "$original" --stats

# A helper: run the demo on a scene and hand back its placement digest, failing loudly if the demo
# itself failed. `set -e` plus a pipeline would hide a non-zero demo exit, so the run is captured
# and its status checked explicitly.
run_demo() { # $1 = scene file, $2 = log file -> echoes the digest
    if ! "$demo_bin" --headless --scene "$1" >"$2" 2>&1; then
        echo "authoring-round-trip.sh: the demo FAILED on $1" >&2
        grep -E "FAIL|claims failed" "$2" >&2 || tail -20 "$2" >&2
        exit 1
    fi
    grep -oE 'scene digest 0x[0-9a-f]+' "$2" | head -1 | awk '{print $3}'
}

say "2/4  the game runs the generated block"
before=$(run_demo "$original" "$work/before.log")
grep -E "^99-the-block: (all|scene digest)" "$work/before.log"

say "3/4  the editor opens it, edits it, and saves it"
"$editor_bin" --smoke --engine "$engine_bin" --scene "$original" --save "$edited"
[ -s "$edited" ] || { echo "authoring-round-trip.sh: the editor wrote no file" >&2; exit 1; }

say "4/4  the game runs the scene the editor saved"
after=$(run_demo "$edited" "$work/after.log")
grep -E "^99-the-block: (all|scene digest)" "$work/after.log"

say "the round trip"
echo "  before (generated) : $before"
echo "  after  (edited)    : $after"
if [ -z "$before" ] || [ -z "$after" ]; then
    echo "authoring-round-trip.sh: could not read a scene digest from the demo" >&2
    exit 1
fi
if [ "$before" = "$after" ]; then
    # The failure this exists to catch: the game ignored the file and rebuilt the level itself, or
    # the editor's save wrote the world it loaded rather than the world it edited. Both look like a
    # perfectly healthy round trip from every other angle.
    echo "authoring-round-trip.sh: the digests MATCH — the edit did not reach the game" >&2
    exit 1
fi

say "authoring round trip: PASS — the editor's edit reached the game"
