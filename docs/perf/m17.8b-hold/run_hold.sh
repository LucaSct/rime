#!/usr/bin/env bash
# run_hold.sh ARM OUT_PREFIX [extra args]  — the WORKTREE binary, pointed at a cooked-dir COPY per
# arm (no file is ever moved in the main tree), with --hold N render-only frames after the measured
# loop. Sidecars as run_one.sh, plus OUT.hold.csv (per-frame).
set -u
arm="$1"; out="$2"; shift 2
S=/tmp/claude-1000/-home-next-projects-rime/2995abcb-eb1f-4334-8573-773660dc1498/scratchpad/fable
bin="$S/wt/build/release/bin/the_block"
case "$arm" in textured) cooked="$S/cooked_T" ;; flat) cooked="$S/cooked_F" ;; *) echo bad arm; exit 2 ;; esac
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
