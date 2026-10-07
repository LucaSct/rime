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

# ── Choosing the baseline report ─────────────────────────────────────────────────────────────
#
# Reports are named <date>-<sample>-<gpu-slug>-<sha>.json. The SHA is there because the name used to
# be one slot per sample per machine per DAY, so a before/after pair measured on one day could not
# both be filed: on 2026-10-06 a control run on main (453866b) and a treatment run on a branch
# (3adeee9) both wanted 2026-10-06-99-the-block-nvidia-geforce-rtx-3060.json, and which number
# survived had to be decided by hand. Reports committed under the old, SHA-less name stay valid
# baselines, so both shapes are matched.
#
# Two traps, both of which silently pick the WRONG baseline instead of failing:
#
#  (a) A trailing-wildcard glob (`*-<slug>*.json`) would take a `...-rtx-3060-ti-...` report for the
#      `...-rtx-3060` machine — another GPU's numbers judged as this one's, the exact mistake the
#      gpu-slug exists to prevent. So the name is matched by an anchored regular expression,
#      ^<date>-<name>-<slug>(-<sha>)?\.json$, with <sha> = [0-9a-f]+(-dirty)?. `ti` is not hex, so
#      it cannot be mistaken for a SHA. A `case` glob cannot express the optional group.
#
#  (b) `sort | tail -1` meant "newest" only because the name began with the date. With a SHA on the
#      end, two reports from the same day sort by SHA, which is arbitrary. So order by the date tag
#      first, then break ties by WHEN THE FILE WAS FILED (commit time; mtime for a report not yet
#      committed). Commit time and not the SHA, because a SHA is a hash — it carries no order at all.
#      Commit time and not the date alone, because the same-day tie is exactly what is being broken.
_re_escape() { printf '%s' "$1" | sed -e 's/[][\.*^$(){}?+|]/\\&/g'; }

# select_baseline <baseline_dir> <name> <slug> <self_path>  — prints the chosen path, or nothing.
select_baseline() {
    local dir="$1" name="$2" slug="$3" self="$4"
    local re="^[0-9]{4}-[0-9]{2}-[0-9]{2}-$(_re_escape "$name")-$(_re_escape "$slug")(-[0-9a-f]+(-dirty)?)?\\.json$"
    local f base ct
    [ -d "$dir" ] || return 0
    for f in "$dir"/*.json; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        printf '%s\n' "$base" | grep -Eq -- "$re" || continue
        [ "$f" = "$self" ] && continue
        ct="$(git log -1 --format=%ct -- "$f" 2>/dev/null || true)"
        [ -z "$ct" ] && ct="$(stat -c %Y "$f" 2>/dev/null || echo 0)"
        printf '%s\t%s\t%s\n' "${base:0:10}" "$ct" "$f"
    done | sort -t "$(printf '\t')" -k1,1 -k2,2n | tail -1 | cut -f3
}

# Runs with no GPU and no build: synthetic filenames in a temp dir, asserted picks.
self_test() {
    local d; d="$(mktemp -d)"
    check() { # description expected-basename-or-empty actual-path
        local got=""
        [ -n "$3" ] && got="$(basename -- "$3")"
        if [ "$got" = "$2" ]; then echo "ok   - $1"; else echo "FAIL - $1: expected '${2}' got '${got}'"; exit 1; fi
    }
    local nm="99-the-block" sl="nvidia-geforce-rtx-3060"
    check "empty directory yields no baseline" "" "$(select_baseline "$d" "$nm" "$sl" "$d/x.json")"
    touch "$d/2026-09-22-${nm}-${sl}.json"
    check "legacy SHA-less name is still selectable" "2026-09-22-${nm}-${sl}.json" \
        "$(select_baseline "$d" "$nm" "$sl" "$d/none.json")"
    touch -d '2026-10-06 10:00' "$d/2026-10-06-${nm}-${sl}-453866b.json"
    touch -d '2026-10-06 11:00' "$d/2026-10-06-${nm}-${sl}-3adeee9-dirty.json"
    check "same-day pair: self excluded, the other chosen" "2026-10-06-${nm}-${sl}-453866b.json" \
        "$(select_baseline "$d" "$nm" "$sl" "$d/2026-10-06-${nm}-${sl}-3adeee9-dirty.json")"
    check "same-day tie broken by filing time, not SHA order" "2026-10-06-${nm}-${sl}-3adeee9-dirty.json" \
        "$(select_baseline "$d" "$nm" "$sl" "$d/none.json")"
    touch "$d/2026-10-08-${nm}-nvidia-geforce-rtx-3060-ti-abc1234.json" "$d/2026-10-08-${nm}-nvidia-geforce-rtx-3060-ti.json"
    check "a 3060-ti report is not chosen for the 3060 slug" "2026-10-06-${nm}-${sl}-3adeee9-dirty.json" \
        "$(select_baseline "$d" "$nm" "$sl" "$d/none.json")"
    touch "$d/2026-09-30-${nm}-${sl}-fffffff.json"
    check "older date with a larger SHA does not beat a newer date" "2026-10-06-${nm}-${sl}-3adeee9-dirty.json" \
        "$(select_baseline "$d" "$nm" "$sl" "$d/none.json")"
    check "another sample is never chosen" "" "$(select_baseline "$d" "11-lit-rooms" "$sl" "$d/none.json")"
    rm -rf "$d"
    echo "perf.sh --self-test: all cases passed"
}

usage() {
    cat <<'EOF'
Rime perf — measure frame/sim time on this machine and write a fingerprinted report.

Usage: scripts/perf.sh [options]
  --preset dev|release    build to measure (default: release — Debug numbers mean nothing)
  --sample NAME           lit-rooms | destructible-wall | the-block | virtual-geometry | all
                          (default: all), or terrain (m19.8e — not in `all`: it needs its
                          generated world, which this script cooks first if it is missing)
  --frames N              measured frames per run (default: the sample's own, 600)
  --width W --height H    render resolution (default: 1920x1080)
  --commit                write the reports into docs/perf/ instead of a scratch dir
  --baseline-dir DIR      where to look for the report to compare against (default: docs/perf)
  --self-test             check baseline selection against synthetic report names; needs no GPU
                          or build, exits non-zero on the first failed case
  -h, --help              show this help

Reports are named <date>-<sample>-<gpu-slug>-<sha>.json. The slug stops a second machine's numbers
overwriting the first's; the SHA stops a second run on the same day overwriting the first, so a
before/after pair can both be filed. Older SHA-less names still count as baselines, and
`git log docs/perf/` reads as the performance history of the engine.
EOF
}

preset="release"; sample="all"; frames=""; width=1920; height=1080; commit=0; run_self_test=0
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
        --self-test) run_self_test=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "perf.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
done

if [ "$run_self_test" -eq 1 ]; then self_test; exit 0; fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# ── The GPU clock is measured DURING the run, not checked before it (m17.3d) ─────────────────
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
# BOTH clock domains have to be measured, and the memory one is the one that bites. `nvidia-smi -lgc`
# pins the graphics clock and says nothing about memory: measured on this box with -lgc 1785 held
# rock-steady for a whole run, the memory clock sat at 810 MHz of 7501 from the first sample to the
# last, never boosting even while the GPU reported 44% utilisation. That is ~11% of peak bandwidth,
# and the passes this report is about (SSR, DDGI, the g-buffer resolves at 1080p) are bandwidth-bound.
# A guard that watched only the core would have passed that machine and called the result a baseline.
#
# The OLD check tried to prove stability up front by reading the idle clocks against their maxima, and
# it was wrong twice over, both measured 2026-09-28:
#   * an idle NVIDIA GPU parks in power state P8 at a low clock NO MATTER WHAT is configured (a GTX
#     1060 reads 139 MHz of a 1911 MHz max while idle, then holds 1898–1911 MHz for the whole
#     measured workload), so an idle reading proves nothing; and
#   * Pascal-class GPUs cannot be pinned at all — `nvidia-smi -lgc`, `-lmc` and `-ac` all print
#     "not supported", then "Treating as warning and moving on", and still exit 0, so an exit-status
#     check on those commands is worthless.
# Pinning was only ever a proxy for stability. This measures the thing itself: the clocks are sampled
# at 20 Hz WHILE the benchmark runs, and the GRAPHICS spread over its own median must stay within 2%
# — a check that works whether or not the card can be pinned. The memory domain is measured and
# reported but does NOT gate: a memory clock that moves while the graphics clock holds still is a real
# and separately interesting signal, but a stable graphics clock is what the frame times ride on. This
# is the same ruling ADR-0041 Ruling 4 makes about an incomparable baseline, applied to a machine that
# stopped being fit to measure.
#
# READ A CLOCK UNDER LOAD OR NOT AT ALL (ADR-0050). A benchmark process is not busy for the whole of
# its lifetime: `lit_rooms --perf` measures its 600 frames in ~1.1 s but spends the time before that
# creating a device, compiling pipelines and uploading assets, with the GPU at 0-1% utilisation.
# Sampling that window on a card that cannot be pinned reads its IDLE clock (139 MHz of a 1911 MHz max
# on the GTX 1060) and would fail every run for a ramp that is not part of the measurement at all. So
# only samples taken while the GPU was working count — and a run that yields too few of those gets no
# verdict rather than a verdict drawn from three samples.
gpu_load_pct=10        # utilisation at or above which a sample belongs to the measured window
gpu_min_samples=10     # fewer loaded samples than this is not a distribution — say so, don't judge
gpu_spread_limit=2.0   # % of the median, the bound ADR-0050 ratifies

gpu_sampler_available() { command -v nvidia-smi >/dev/null 2>&1; }

# Start ONE long-lived nvidia-smi writing `graphics, memory, utilisation` at 20 Hz into $1, and echo
# its pid. One process rather than one per sample deliberately: this script refuses to measure on a
# box that is not quiet (below), so a guard that forked a process twice a second would be contending
# with the very run it is judging (the sampler itself measures 0.0% CPU). `-i 0` keeps the old
# check's "first GPU" scope — on a two-card box the idle one would otherwise read as a clock
# collapse that never happened.
start_gpu_sampler() {
    nvidia-smi -i 0 --query-gpu=clocks.gr,clocks.mem,utilization.gpu \
        --format=csv,noheader,nounits -lms 50 > "$1" 2>/dev/null &
    echo $!
}

stop_gpu_sampler() {
    kill "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
}

# Read a sorted column of numbers on stdin; print "min median max spread_pct".
col_stats() {
    awk '
        { a[NR] = $1 }
        END {
            if (NR == 0) exit 1
            min = a[1]; max = a[NR]
            med = (NR % 2) ? a[int(NR / 2) + 1] : (a[int(NR / 2)] + a[int(NR / 2) + 1]) / 2
            printf "%.0f %.1f %.0f %.1f\n", min, med, max, (max - min) / med * 100
        }'
}

# Reduce a sample log to one line — graphics min/median/max/spread, memory min/median/max/spread,
# total samples, loaded samples, and the verdict `true`, `false` or `null` — or print nothing at all
# when there was no readable sample. `null` is the honest answer for a run too short to judge: an
# absent GPU, a driver that answers [N/A] and a benchmark that finished in half a second must none of
# them produce a verdict, and none of them may gate the run.
gpu_clock_stats() {
    local log="$1" loaded total
    total="$(awk -F'[, ]+' '$1 ~ /^[0-9]+$/ { n++ } END { print n + 0 }' "$log")"
    loaded="$(awk -F'[, ]+' -v t="$gpu_load_pct" \
        '$1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ && $3 + 0 >= t { print $1, $2 }' "$log")"
    [ -n "$loaded" ] || return 0
    local n gmin gmed gmax gspread mmin mmed mmax mspread stable=""
    n="$(printf '%s\n' "$loaded" | grep -c .)"
    read -r gmin gmed gmax gspread <<<"$(printf '%s\n' "$loaded" | awk '{ print $1 }' | sort -n | col_stats)"
    read -r mmin mmed mmax mspread <<<"$(printf '%s\n' "$loaded" | awk '{ print $2 }' | sort -n | col_stats)"
    # An unreadable spread must never read as a pass. col_stats cannot fail on a non-empty column,
    # but a verdict that defaults to `true` when a number is missing is the exact shape of the bug
    # this whole section exists because of, so it is checked rather than assumed.
    case "$gspread" in ''|*[!0-9.]*) stable="null"; gspread="null" ;; esac
    if [ "${stable:-}" = "null" ]; then
        :
    elif [ "$n" -lt "$gpu_min_samples" ]; then
        stable="null"
    elif awk -v s="$gspread" -v l="$gpu_spread_limit" 'BEGIN { exit (s > l) ? 0 : 1 }'; then
        stable="false"
    else
        stable="true"
    fi
    printf '%s %s %s %s %s %s %s %s %s %s %s\n' \
        "$gmin" "$gmed" "$gmax" "$gspread" "$mmin" "$mmed" "$mmax" "$mspread" "$total" "$n" "$stable"
}

# Splice a `gpu_clocks` object into the pretty-printed report just before its root closing brace.
# The report already carries its other top-level keys, so the object is added with a leading comma.
# `samples` is in the report on purpose: a spread is only as meaningful as the number of readings
# behind it, and a reader comparing two reports must be able to see that without rerunning anything.
inject_gpu_clocks() {
    local report="$1" gmin="$2" gmed="$3" gmax="$4" gspread="$5" \
          mmin="$6" mmed="$7" mmax="$8" mspread="$9" total="${10}" loaded="${11}" stable="${12}"
    awk -v gm="$gmin" -v gd="$gmed" -v gx="$gmax" -v gs="$gspread" \
        -v mm="$mmin" -v md="$mmed" -v mx="$mmax" -v ms="$mspread" \
        -v tot="$total" -v ld="$loaded" -v st="$stable" '
        { line[NR] = $0; if ($0 ~ /^[[:space:]]*}[[:space:]]*$/) last = NR }
        END {
            for (i = 1; i <= NR; i++) {
                if (i == last - 1) { print line[i] ","; continue }
                if (i == last) {
                    print "  \"gpu_clocks\": {"
                    printf "    \"graphics_min\": %s,\n", gm
                    printf "    \"graphics_median\": %s,\n", gd
                    printf "    \"graphics_max\": %s,\n", gx
                    printf "    \"graphics_spread_pct\": %s,\n", gs
                    printf "    \"memory_min\": %s,\n", mm
                    printf "    \"memory_median\": %s,\n", md
                    printf "    \"memory_max\": %s,\n", mx
                    printf "    \"memory_spread_pct\": %s,\n", ms
                    printf "    \"samples_total\": %s,\n", tot
                    printf "    \"samples_under_load\": %s,\n", ld
                    printf "    \"stable\": %s\n", st
                    print "  }"
                }
                print line[i]
            }
        }' "$report" > "$report.tmp" && mv "$report.tmp" "$report"
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
#
# TWO THINGS THIS GETS RIGHT THAT THE OBVIOUS `ps -eo pcpu,comm` VERSION DID NOT.
#
# It samples an INTERVAL. `ps`'s %CPU is cputime/elapsed over the process's whole LIFETIME, which
# is the wrong question twice over: a long-lived process (an editor's language server, a browser,
# a sibling session's node) that starts spinning right now climbs towards 50 over minutes and may
# never reach it during a 20-second run, while a process that burned a core an hour ago and has
# been idle since still reads high and fails an innocent run. Deltas of utime+stime over one second
# are the instantaneous number the guard is actually asking for.
#
# And it compares against TRUNCATED names. The kernel stores comm in 16 bytes, so `ps -o comm`
# prints at most 15 characters: this script's own `destructible_wall` (17) appears as
# `destructible_wa` and an allow-list containing the full name never matches it — which would have
# made every `--sample destructible-wall` and every `--sample all` run report ITSELF as a
# competitor and fail. Found in review before the path was ever exercised, because the reports
# committed so far predate the watcher.
foreign_mine="the_block lit_rooms destructible_wall virtual_geometry terrain_flythrough rime_ nvidia-smi perf.sh"

# Linux only today: the sampler reads /proc. Kept as its own predicate so that "no contention was
# detected" and "contention could not be detected" are never the same answer — an absent sampler
# produces an empty log, and an empty log is exactly what a quiet machine looks like.
foreign_sampler_available() { [ -r /proc/stat ]; }

foreign_busy() {
    if ! foreign_sampler_available; then
        return 0
    fi
    local a b; a="$(mktemp)"; b="$(mktemp)"
    proc_cpu_snapshot > "$a"; sleep 1; proc_cpu_snapshot > "$b"
    foreign_compare "$a" "$b" 1
    rm -f "$a" "$b"
}

# The same comparison, over an interval the CALLER paced. Split out so the watcher can keep one
# rolling snapshot and sweep /proc ONCE per interval instead of twice — a watcher that reads six
# hundred files every second, inside the measurement it exists to keep clean, is a competitor for
# the cache if not for a core. The one-shot form above still pays for two, because a pre-check runs
# before anything is being measured.
foreign_compare() {
    local hz; hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
    awk -v hz="$hz" -v mine="$foreign_mine" -v secs="$3" '
        BEGIN { n = split(mine, m, " ") }
        NR == FNR { was[$1] = $3; next }
        {
            d = $3 - (($1 in was) ? was[$1] : $3)   # a process born during the window: no delta
            if (d <= 0) next
            pct = 100.0 * d / (hz * secs)
            if (pct <= 50) next
            for (i = 1; i <= n; ++i) {
                # comm is truncated to 15 chars, so compare against a truncated allow-list entry.
                if (index($2, substr(m[i], 1, 15)) == 1) next
            }
            printf "  %s at %.0f%% CPU\n", $2, pct
        }' "$1" "$2"
}

# One /proc sweep per interval, forever. The rolling snapshot is what makes it cheap.
watch_box() {
    local prev cur; prev="$(mktemp)"; cur="$(mktemp)"
    proc_cpu_snapshot > "$prev"
    while true; do
        sleep 2
        proc_cpu_snapshot > "$cur"
        foreign_compare "$prev" "$cur" 2
        mv -f "$cur" "$prev"
    done
}

# pid, comm, cpu-ticks — one line per process, from ONE awk. Deliberately not a shell loop over
# `/proc/*/stat`: that forks a process per process, twice a second, inside the very measurement it
# is supposed to leave undisturbed. A watcher that costs a core is a competitor.
proc_cpu_snapshot() {
    awk 'BEGIN {
        while (("ls -d /proc/[0-9]*/stat 2>/dev/null" | getline f) > 0) {
            if ((getline line < f) > 0) {
                n = split(line, F, " ")
                # utime and stime are the 12th and 13th fields AFTER comm. Indexed from the closing
                # paren rather than from the start, because comm may itself contain spaces.
                base = 0
                for (i = 1; i <= n; ++i) if (F[i] ~ /\)$/) { base = i; break }
                if (base > 0) {
                    comm = F[2]; sub(/^\(/, "", comm); sub(/\)$/, "", comm)
                    print F[1], comm, F[base + 12] + F[base + 13]
                }
            }
            close(f)
        }
    }'
}

check_box_quiet() {
    if ! foreign_sampler_available; then
        echo "perf.sh: this platform has no /proc, so the CPU-contention guard is INACTIVE." >&2
        echo "  Nothing here can tell you the box was idle — check it yourself before believing" >&2
        echo "  the numbers, and say in the PR that the run was unguarded." >&2
        return 0
    fi
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

# EVERY run writes to a staging directory first, `--commit` or not. The verdict on whether the box
# stayed idle only exists once the run is over, and `run_one` writes its report before that — so
# committing straight into docs/perf/ meant a contaminated run failed with exit 1 AND left the
# contaminated report filed, overwriting the good one it was meant to be compared against. The
# comment above says "failed rather than filed"; this is what makes that true.
outdir="$(mktemp -d)"
if [ "$commit" -eq 1 ]; then
    final_dir="docs/perf"
    mkdir -p "$final_dir"
else
    final_dir=""
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
clock_unstable=0
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

    local out="${outdir}/${date_tag}-${name}-${slug}-${sha}.json"
    # Exclude the report this run is about to FILE, not the staging path — otherwise a second run
    # on the same date would judge itself against the copy of itself it is about to replace.
    # The SHA is in the name, so this is the exact file, not a date slot.
    local self="${baseline_dir}/${date_tag}-${name}-${slug}-${sha}.json"
    local latest
    latest="$(select_baseline "$baseline_dir" "$name" "$slug" "$self")"

    echo "── ${name} on ${gpu} ──"
    # The clock sampler is the BACKGROUND job and the benchmark stays in the foreground, so the run
    # keeps the terminal, the signal handling and the exit status it always had, and the sampler is
    # something this function starts and stops around it. The 12-frame probe above is deliberately
    # not sampled: its numbers are never used for anything.
    local clock_log="" sampler_pid=""
    if gpu_sampler_available; then
        clock_log="$(mktemp)"
        sampler_pid="$(start_gpu_sampler "$clock_log")"
    fi
    if [ -n "$latest" ]; then
        echo "  baseline: ${latest}"
        "${bin}/${exe}" --perf --width "$width" --height "$height" \
            --out "$out" --baseline "$latest" "${extra[@]}" "$@" || status=1
    else
        echo "  baseline: none yet for this machine — this run establishes one"
        "${bin}/${exe}" --perf --width "$width" --height "$height" \
            --out "$out" "${extra[@]}" "$@" || status=1
    fi
    [ -n "$sampler_pid" ] && stop_gpu_sampler "$sampler_pid"

    if [ -n "$clock_log" ]; then
        local clock_line
        clock_line="$(gpu_clock_stats "$clock_log")"
        rm -f "$clock_log"
        if [ -n "$clock_line" ] && [ -s "$out" ]; then
            local gmin gmed gmax gspread mmin mmed mmax mspread total loaded stable
            read -r gmin gmed gmax gspread mmin mmed mmax mspread total loaded stable <<<"$clock_line"
            inject_gpu_clocks "$out" "$gmin" "$gmed" "$gmax" "$gspread" \
                "$mmin" "$mmed" "$mmax" "$mspread" "$total" "$loaded" "$stable"
            echo "  gpu clock: ${gmed} MHz median, ${gspread}% spread over ${loaded} of ${total} samples (stable=${stable})"
            if [ "$stable" = "false" ]; then
                clock_unstable=1
                status=1
                cat >&2 <<EOF

perf.sh: the GPU graphics clock was NOT stable during this run.

  graphics clock: ${gmin}–${gmax} MHz — ${gspread}% spread of a ${gmed} MHz median (limit ${gpu_spread_limit}%)
  memory clock:   ${mmin}–${mmax} MHz — ${mspread}% spread (reported, not gating)
  measured from ${loaded} samples taken at ≥${gpu_load_pct}% utilisation, of ${total} taken in all

  A graphics clock that moves more than ${gpu_spread_limit}% over the run means this report measures the
  clock ramp as much as the engine, so the numbers are not comparable against a stable baseline.
  Re-run once the card settles — and check cooling and power limits, because the thermal and power
  governors move the graphics clock even on a card that cannot be pinned.
EOF
            elif [ "$stable" = "null" ]; then
                # Not a failure and NOT a pass: too short a loaded window to call. The report keeps
                # the numbers and a null verdict, so a reader can see the precondition was not
                # established rather than assume it was.
                echo "" >&2
                echo "perf.sh: only ${loaded} sample(s) at ≥${gpu_load_pct}% GPU utilisation — too few to judge" >&2
                echo "  clock stability, so this report carries \"stable\": null. It is NOT a verified-stable run." >&2
            fi
        fi
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
watch_box > "$contention_log" 2>/dev/null &
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
    # m18.3d. ADR-0043 gate 4's complexity sweep: candidates, selected triangles, CPU submission and
    # GPU time at a fixed projected size. Unlike the samples above it needs no cooked asset — it
    # generates its own quadtree, because the sweep's variable is the size of the CUT and a cooked
    # mesh cannot move that without moving five other things with it. `--sweep` prints every row;
    # the committed report is the reference depth, so `git log docs/perf/` still reads as a series.
    virtual-geometry)  run_one virtual_geometry 14-virtual-geometry --sweep ;;
    # m19.8e (ADR-0073). The terrain travel budget: walk / vehicle / aircraft fly-throughs plus the
    # envelope speeds, over a generated 4 km world cooked by `rime terrain-world`. Kept out of
    # `all` because the world is ~30 MB of generated files; make_world.sh cooks it on first use.
    terrain)
        [ -f build/terrain-perf-world/world/terrain_perf.terrainworld ] ||
            samples/15-terrain/make_world.sh >/dev/null
        run_one terrain_flythrough 15-terrain --envelope ;;
    all)
        run_one lit_rooms 11-lit-rooms
        run_one destructible_wall 10-destructible-wall
        run_one the_block 99-the-block
        run_one virtual_geometry 14-virtual-geometry --sweep
        ;;
    *) echo "perf.sh: unknown sample '$sample' (try --help)" >&2; exit 2 ;;
esac

contended=0
if [ -n "${contention_watcher:-}" ]; then
    # A DEAD WATCHER MUST NOT READ AS AN IDLE BOX. The watch is a subshell; if it ever died — a
    # failed `ps`, an OOM kill, a stray signal — its log is empty, and an empty log is exactly what
    # "the machine stayed quiet" looks like. So its liveness is checked before its silence is
    # believed, and an absent watcher is reported as unknown rather than as clean.
    if ! foreign_sampler_available; then
        echo "" >&2
        echo "perf.sh: the box was NOT watched (no sampler on this platform)." >&2
        echo "  The reports are filed, but nothing checked for a competitor for the CPU." >&2
    elif ! kill -0 "$contention_watcher" 2>/dev/null; then
        echo "" >&2
        echo "perf.sh: the contention watcher died during this run — the box was NOT watched." >&2
        echo "  Treat these reports as unverified for CPU contention and re-run." >&2
        contended=1
    fi
    kill "$contention_watcher" 2>/dev/null || true
    trap - EXIT
    if [ -s "$contention_log" ]; then
        echo "" >&2
        echo "perf.sh: THE BOX DID NOT STAY IDLE. These reports are not trustworthy:" >&2
        sort -u "$contention_log" | head -10 >&2
        echo "  Re-run once the machine is free. A number measured against a competitor for the" >&2
        echo "  CPU is a measurement of the competitor." >&2
        contended=1
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

# File the staged reports — or don't. A report the box was not quiet for is left where it was
# written and named, so it can still be looked at, but it does not become the committed history.
if [ -n "$final_dir" ]; then
    if [ "$clock_unstable" -eq 0 ] && { [ "$contended" -eq 0 ] || [ -n "${RIME_PERF_ALLOW_BUSY_BOX:-}" ]; }; then
        for f in "$outdir"/*.json; do
            [ -e "$f" ] || continue
            mv "$f" "$final_dir/"
            echo "  filed $final_dir/$(basename "$f")"
        done
    else
        echo "" >&2
        echo "perf.sh: NOT filing into ${final_dir} — the box was not idle or a GPU clock moved." >&2
        echo "  The reports are in ${outdir} if you want to look at them." >&2
    fi
fi

if [ "$status" -ne 0 ]; then
    echo "perf.sh: at least one run failed its perf gate." >&2
fi
exit "$status"
