#!/usr/bin/env bash
# run_hold_r2.sh ARM OUT_PREFIX [extra args]  — Sonnet retake of the fable run_hold.sh, pointed at
# a fresh worktree (the original session's /tmp scratchpad is gone). Same protocol: the WORKTREE
# binary, a per-arm cooked-dir COPY (never the main tree's manifest.txt), --hold render-only frames
# after the measured loop. Sidecars: .clk (nvidia-smi trace), .cpu (foreign-CPU %), .hold.csv
# (per-frame hold data), .log, .meta.
set -u
arm="$1"; out="$2"; shift 2
HARNESS=/tmp/devpass-code/rime-m17.8b-harness
bin="$HARNESS/wt/build/release/bin/the_block"
case "$arm" in textured) cooked="$HARNESS/cooked_T" ;; flat) cooked="$HARNESS/cooked_F" ;; *) echo bad arm; exit 2 ;; esac
cleanup() { [ -n "${clk_pid:-}" ] && kill "$clk_pid" 2>/dev/null; [ -n "${cpu_pid:-}" ] && kill "$cpu_pid" 2>/dev/null; }
trap cleanup EXIT INT TERM
nvidia-smi --query-gpu=timestamp,clocks.gr,clocks.mem,utilization.gpu,utilization.memory,power.draw,pstate,clocks_throttle_reasons.active \
    --format=csv,noheader,nounits -lms 200 > "$out.clk" 2>/dev/null &
clk_pid=$!
(
  hz=$(getconf CLK_TCK)
  read_sys() { awk '/^cpu /{print $2+$3+$4+$7+$8+$9}' /proc/stat; }
  read_me()  { local p; p=$(pgrep -x the_block | head -1); [ -n "$p" ] && awk '{print $14+$15}' /proc/$p/stat 2>/dev/null || echo 0; }
  ps0=$(read_sys); pm0=$(read_me)
  while sleep 1; do
    ps1=$(read_sys); pm1=$(read_me); ds=$((ps1-ps0)); dm=$((pm1-pm0)); [ "$dm" -lt 0 ] && dm=0
    echo "$(date +%s.%N | cut -c1-14) $(( (ds-dm)*100/hz )) $(( dm*100/hz ))"; ps0=$ps1; pm0=$pm1
  done
) > "$out.cpu" 2>/dev/null &
cpu_pid=$!
t0=$(date +%s.%N)
"$bin" --perf --width 1920 --height 1080 --cooked "$cooked" --out "$out.json" --hold-out "$out.hold.csv" "$@" > "$out.log" 2>&1
rc=$?
t1=$(date +%s.%N)
cleanup; wait "$clk_pid" "$cpu_pid" 2>/dev/null
bound=$(python3 -c "import json; print(json.load(open('$out.json'))['ledger'].get('ground.materials_bound','ABSENT'))" 2>/dev/null || echo NOJSON)
echo "$arm rc=$rc bound=$bound wall=$(python3 -c "print(round($t1-$t0,1))")s" | tee "$out.meta"
