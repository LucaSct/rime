#!/usr/bin/env bash
# Rime — take a hardware performance report and, optionally, commit it to docs/perf/.
#
# The other half of ADR-0035 §2. `scripts/build.sh` runs the proofs CI gates: counts, on lavapipe,
# forever. This script runs the ones CI CANNOT gate — wall-clock distributions on a real GPU — and
# writes a fingerprinted JSON that says which machine produced them.
#
# It is deliberately a script rather than a CI job. A self-hosted runner on one desk machine makes
# every merge hostage to that box's uptime and launders thermal noise into red/green; the ADR names
# that trade-off and takes the procedural side, mitigated by committing the reports so an absent
# measurement is visible in review rather than merely absent.
#
# Two things it does that running the sample by hand does not:
#   * it stamps RIME_PERF_COMMIT, so the report says which tree it measured (the engine does not
#     shell out to git to describe itself, and a compile-time bake goes stale the moment you commit
#     without reconfiguring — which is exactly when a wrong answer would be most convincing); and
#   * it defaults to the RELEASE build, because a Debug or sanitizer binary is several times slower
#     and the fingerprint would (correctly) refuse to compare it against anything useful.
set -euo pipefail

usage() {
    cat <<'EOF'
Rime perf — measure frame/sim time on this machine and write a fingerprinted report.

Usage: scripts/perf.sh [options]
  --preset dev|release    build to measure (default: release — Debug numbers mean nothing)
  --sample NAME           lit-rooms | destructible-wall | the-block | all  (default: all)
  --frames N              measured frames per run (default: the sample's own, 600)
  --width W --height H    render resolution (default: 1920x1080)
  --commit                write the reports into docs/perf/ instead of a scratch dir
  --baseline-dir DIR      where to look for the report to compare against (default: docs/perf)
  -h, --help              show this help

Reports are named <date>-<sample>-<gpu-slug>.json, so a second machine's numbers never overwrite
the first's, and `git log docs/perf/` reads as the performance history of the engine.
EOF
}

preset="release"; sample="all"; frames=""; width=1920; height=1080; commit=0
baseline_dir="docs/perf"
while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   preset="${2:?--preset needs a value}"; shift 2 ;;
        --preset=*) preset="${1#*=}"; shift ;;
        --sample)   sample="${2:?--sample needs a value}"; shift 2 ;;
        --sample=*) sample="${1#*=}"; shift ;;
        --frames)   frames="${2:?--frames needs a value}"; shift 2 ;;
        --frames=*) frames="${1#*=}"; shift ;;
        --width)    width="${2:?--width needs a value}"; shift 2 ;;
        --height)   height="${2:?--height needs a value}"; shift 2 ;;
        --commit)   commit=1; shift ;;
        --baseline-dir) baseline_dir="${2:?--baseline-dir needs a value}"; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "perf.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

bin="build/${preset}/bin"
if [ ! -x "${bin}/lit_rooms" ]; then
    echo "perf.sh: no ${preset} build at ${bin} — run scripts/build.sh --preset ${preset} first" >&2
    exit 1
fi

# The tree being measured. A dirty tree is reported as such rather than silently attributed to the
# last commit: a report that claims a SHA it does not match is worse than one that admits it.
sha="$(git rev-parse --short HEAD)"
if ! git diff --quiet HEAD 2>/dev/null; then
    sha="${sha}-dirty"
fi
export RIME_PERF_COMMIT="$sha"

if [ "$commit" -eq 1 ]; then
    outdir="docs/perf"
    mkdir -p "$outdir"
else
    outdir="$(mktemp -d)"
    echo "perf.sh: writing to ${outdir} (pass --commit to write into docs/perf/)"
fi
date_tag="$(date -u +%Y-%m-%d)"

# A filesystem-safe slug for the GPU, so two machines' reports coexist in one directory. Derived
# from the sample's own first line, which prints the adapter name the RHI selected — asking the
# driver a second way could disagree with what the engine actually ran on.
slug_of() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | sed -e 's/[^a-z0-9]\+/-/g' -e 's/^-//' -e 's/-$//'
}

status=0
run_one() {
    local exe="$1" name="$2"; shift 2
    local extra=()
    [ -n "$frames" ] && extra+=(--frames "$frames")

    # Which report is this run's baseline is a question about the GPU, and the only authority on
    # which GPU the engine picked is the engine — asking the driver separately (vulkaninfo,
    # nvidia-smi) can name a different device on a box with two of them, which this one has. So a
    # 12-frame probe writes a throwaway report, we read the adapter name out of THAT, and the
    # measured run is then told exactly which baseline to judge itself against.
    #
    # The probe fails its own gate (twelve frames is not evidence about a tail, and the gate says
    # so) — deliberately ignored here, because its numbers are never used for anything.
    local probe; probe="$(mktemp)"
    "${bin}/${exe}" --perf --frames 12 --warmup 2 --width 320 --height 180 --out "$probe" "$@" \
        >/dev/null 2>&1 || true
    local gpu; gpu="$(sed -n 's/^[[:space:]]*"gpu":[[:space:]]*"\(.*\)".*$/\1/p' "$probe" | head -1)"
    rm -f "$probe"
    [ -z "$gpu" ] && gpu="unknown-gpu"
    local slug; slug="$(slug_of "$gpu")"

    local out="${outdir}/${date_tag}-${name}-${slug}.json"
    local latest
    latest="$(ls -1 "${baseline_dir}"/*-"${name}"-"${slug}".json 2>/dev/null \
              | grep -vxF -- "$out" | tail -1 || true)"

    echo "── ${name} on ${gpu} ──"
    if [ -n "$latest" ]; then
        echo "  baseline: ${latest}"
        "${bin}/${exe}" --perf --width "$width" --height "$height" \
            --out "$out" --baseline "$latest" "${extra[@]}" "$@" || status=1
    else
        echo "  baseline: none yet for this machine — this run establishes one"
        "${bin}/${exe}" --perf --width "$width" --height "$height" \
            --out "$out" "${extra[@]}" "$@" || status=1
    fi
}

case "$sample" in
    lit-rooms)         run_one lit_rooms 11-lit-rooms ;;
    destructible-wall) run_one destructible_wall 10-destructible-wall ;;
    # m13.p. The vision demo, and the only sample whose numbers the milestone's "playable frame
    # rate" clause is actually about. It needs its cooked `.rdest` set — the CTest fixtures produce
    # it (`ctest -R block_demo_cook` in the build dir), and the run will refuse to start without it
    # rather than measure a block that is not there.
    the-block)         run_one the_block 99-the-block ;;
    all)
        run_one lit_rooms 11-lit-rooms
        run_one destructible_wall 10-destructible-wall
        run_one the_block 99-the-block
        ;;
    *) echo "perf.sh: unknown sample '$sample' (try --help)" >&2; exit 2 ;;
esac

if [ "$status" -ne 0 ]; then
    echo "perf.sh: at least one run failed its perf gate." >&2
fi
exit "$status"
