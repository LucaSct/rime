#!/usr/bin/env bash
set -u
S=/tmp/claude-1000/-home-next-projects-rime/2995abcb-eb1f-4334-8573-773660dc1498/scratchpad/fable
outdir="$1"; order="$2"; shift 2; mkdir -p "$outdir"; i=0
for a in $order; do i=$((i+1)); case "$a" in T) arm=textured ;; F) arm=flat ;; esac
  sleep 3; "$S/run_hold.sh" "$arm" "$outdir/r$(printf %02d $i)-$a" "$@"; done
echo "series done: $outdir"
