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

# ── The GPU must not be allowed to park itself (m17.3d) ──────────────────────────────────────
#
# Found the hard way while taking M17's re-baseline. `99-the-block` is CPU-bound — the simulation is
# ~24 ms of a ~35 ms frame — so the GPU is idle most of every frame and the driver's power governor
# concludes it has nothing to do. Measured directly during a 600-frame run: core 1837 -> 210 MHz and
# MEMORY 7501 -> 405 MHz, an 18x collapse in bandwidth, part-way through the run and staying there.
#
# What that does to a report is worse than making it slow, because it makes it INCONSISTENT. Two
# identical 600-frame runs minutes apart agreed on frame p99 (41.48 / 41.58) and disagreed by 2x on
# the same pass: `ssr-resolve` p50 1.842 vs 0.856 ms. A committed baseline measured like that
# encodes the governor's mood, and every future comparison against it inherits that.
#
# BOTH clock domains have to be checked, and the memory one is the one that bites. `nvidia-smi -lgc`
# pins the graphics clock and says nothing about memory: measured on this box with -lgc 1785 held
# rock-steady for a whole run, the memory clock sat at 810 MHz of 7501 from the first sample to the
# last, never boosting even while the GPU reported 44% utilisation. That is ~11% of peak bandwidth,
# and the passes this report is about (SSR, DDGI, the g-buffer resolves at 1080p) are bandwidth-bound.
# A guard that watched only the core would have passed that machine and called the result a baseline.
#
# So this refuses to run rather than producing a number nobody can trust — the same ruling
# ADR-0041 Ruling 4 makes about an incomparable baseline, applied one step earlier to an
# unmeasurable machine. Override deliberately if you are measuring something the parking cannot
# reach (a GPU-bound sample) or on a machine where clocks cannot be pinned.
check_gpu_clocks() {
    command -v nvidia-smi >/dev/null 2>&1 || return 0   # not an NVIDIA box; nothing to check

    # A pinned domain sits at its locked floor even while idle, so a clock far below its own maximum
    # means the governor is still in charge of that domain. Reported per domain, because being told
    # "the GPU is not pinned" when the core is fine and the memory is not sends you to the wrong fix.
    local unpinned=""
    local cur_gr max_gr cur_mem max_mem
    read -r cur_gr max_gr cur_mem max_mem <<<"$(nvidia-smi \
        --query-gpu=clocks.current.graphics,clocks.max.graphics,clocks.current.memory,clocks.max.memory \
        --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ' | tr ',' ' ')"

    domain_unpinned() {   # $1=name $2=current $3=max -> echoes a description when the governor owns it
        case "$2$3" in ''|*[!0-9]*) return 0 ;; esac    # unreadable: do not invent a verdict
        [ "$3" -gt 0 ] || return 0
        [ $(( $2 * 2 )) -ge "$3" ] && return 0
        printf '  %s clock: %s MHz of %s MHz max\n' "$1" "$2" "$3"
    }
    unpinned+="$(domain_unpinned graphics "$cur_gr"  "$max_gr")"
    unpinned+="$(domain_unpinned memory   "$cur_mem" "$max_mem")"
    [ -z "$unpinned" ] && return 0

    # Deliberately BELOW the maximum boost clock for the core. Pinning at max invites the thermal
    # governor to take over instead of the idle one, which reintroduces exactly the variance being
    # removed; a clock the card can hold indefinitely is what makes two runs comparable. Memory gets
    # no such treatment — GDDR6 has one supported clock on this class of card, and it is the top one.
    local pin=$(( max_gr * 85 / 100 ))
    cat >&2 <<EOF
perf.sh: the GPU is not clock-pinned.

${unpinned}

  This sample is CPU-bound, so the driver parks the GPU mid-run and the report becomes a
  measurement of the power governor rather than of the engine. Pin BOTH domains first — locking
  the graphics clock alone leaves memory free to sit at its idle state all run:

      sudo nvidia-smi -pm 1
      sudo nvidia-smi -lgc ${pin},${pin}
      sudo nvidia-smi -lmc ${max_mem}

  and afterwards, to hand the GPU back to the governor:

      sudo nvidia-smi -rgc
      sudo nvidia-smi -rmc

  Set RIME_PERF_ALLOW_UNPINNED_CLOCKS=1 to measure anyway — and say so in the PR, because the
  numbers are not comparable against a pinned baseline.
EOF
    return 1
}

# ── …and the CPU must not be shared either (m17.5) ───────────────────────────────────────────
#
# The sibling to the clock guard, and learned the same way: `99-the-block` is CPU-bound, so a rival
# for cores is a rival for the number. A second Claude session on this machine ran a determinism
# sweep — three CPU-bound demos at once, touching nothing in this repo — while an A/B series was
# being taken. It produced a clean-looking, monotonic 1-3 ms drift across five runs that read
# exactly like a regression, and a HEAD control taken afterwards on the IDENTICAL committed tree
# came back at 52.37 ms p99 against the 28.69 it had measured twenty minutes earlier. p50 barely
# moved; the tail was destroyed. A blown tail with a healthy median and pinned, cool clocks is the
# signature.
#
# Named processes are excluded because they are this script's own work; everything else above the
# threshold is a competitor, whoever started it.
foreign_busy() {
    ps -eo pcpu,comm --no-headers 2>/dev/null | awk '
        $1 > 50 && $2 !~ /^(the_block|lit_rooms|destructible_wall|rime_|nvidia-smi|perf\.sh)/ {
            printf "  %s at %s%% CPU\n", $2, $1
        }'
}

check_box_quiet() {
    local busy; busy="$(foreign_busy)"
    [ -z "$busy" ] && return 0
    cat >&2 <<EOF
perf.sh: this machine is not idle — something else is using the CPU.

${busy}

  These samples are CPU-bound, so another process at full tilt does not merely slow the run, it
  moves the TAIL while leaving the median about where it was — which is indistinguishable from a
  real regression in the committed report.

  Wait for the box to go quiet, or set RIME_PERF_ALLOW_BUSY_BOX=1 to measure anyway and say so in
  the PR, because the numbers are not comparable against a report taken on an idle machine.
EOF
    return 1
}

if [ -z "${RIME_PERF_ALLOW_UNPINNED_CLOCKS:-}" ]; then
    check_gpu_clocks || exit 3
fi
if [ -z "${RIME_PERF_ALLOW_BUSY_BOX:-}" ]; then
    check_box_quiet || exit 4
fi

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

# Checking once, before the run, would have caught today's case only by luck: the contention began
# part-way through a series. So the box is watched for the WHOLE run and a report measured against a
# competitor is failed rather than filed — the same ruling as an incomparable baseline, applied to a
# machine that stopped being fit to measure half-way through.
#
# The watcher runs even when RIME_PERF_ALLOW_BUSY_BOX is set. The override is a decision to measure
# a busy machine anyway; it is not a decision to stop KNOWING the machine was busy. Gating the
# watch on the same variable as the refusal made every overridden run report itself as clean — so
# the one strategy the override exists for ("run repeatedly through someone else's build, keep the
# runs that happened to land in a gap") could not tell a gap from a collision. Overridden, a dirty
# run is reported and not failed; unoverridden, it is failed.
contention_log="$(mktemp)"
( while true; do foreign_busy; sleep 2; done ) > "$contention_log" 2>/dev/null &
contention_watcher=$!
trap 'kill "$contention_watcher" 2>/dev/null' EXIT

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

if [ -n "${contention_watcher:-}" ]; then
    kill "$contention_watcher" 2>/dev/null || true
    trap - EXIT
    if [ -s "$contention_log" ]; then
        echo "" >&2
        echo "perf.sh: THE BOX DID NOT STAY IDLE. These reports are not trustworthy:" >&2
        sort -u "$contention_log" | head -10 >&2
        echo "  Re-run once the machine is free. A number measured against a competitor for the" >&2
        echo "  CPU is a measurement of the competitor." >&2
        if [ -z "${RIME_PERF_ALLOW_BUSY_BOX:-}" ]; then
            status=1
        else
            echo "  (RIME_PERF_ALLOW_BUSY_BOX is set, so this is a warning and not a failure —" >&2
            echo "   discard this run yourself, or say in the PR why it stands.)" >&2
        fi
    elif [ -n "${RIME_PERF_ALLOW_BUSY_BOX:-}" ]; then
        echo "perf.sh: RIME_PERF_ALLOW_BUSY_BOX was set, but the box stayed idle for the whole run."
    fi
fi
rm -f "$contention_log"

if [ "$status" -ne 0 ]; then
    echo "perf.sh: at least one run failed its perf gate." >&2
fi
exit "$status"
