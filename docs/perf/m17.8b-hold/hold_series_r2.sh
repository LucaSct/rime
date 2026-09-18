#!/usr/bin/env bash
set -u
RUN=/tmp/devpass-code/rime-m17.8b-harness/run_hold_r2.sh
outdir="$1"; order="$2"; shift 2; mkdir -p "$outdir"; i=0
for a in $order; do i=$((i+1)); case "$a" in T) arm=textured ;; F) arm=flat ;; esac
  sleep 3; "$RUN" "$arm" "$outdir/r$(printf %02d $i)-$a" "$@"; done
echo "series done: $outdir"
