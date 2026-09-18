#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Reproduce every number ADR-0041's 2026-09-17 amendment cites from the committed runs.csv alone —
no raw per-frame data, no re-running the harness. Prints (a) each session's per-metric A/B and A/A
adjacent-pair lists and medians, in the same form `analyze_unfiltered.py <session> 100` prints them
(that script's own PRIMARY ESTIMATOR section), (b) the pooled tables exactly as `pool_sessions.py`
prints them (byte-for-byte, including its incidental 4-decimal per-pair rounding), and (c) the same
hold-loop pooled table recomputed at full precision — the version ADR-0041's own table cites, which
differs from (b) in exactly one cell (ssr_p50's ratio). Run: python3 pool.py
"""
import csv
import os
import statistics as st
from collections import defaultdict

SESSIONS = ['h1', 'h2', 'r2']
HOLD_STATS = [
    'frame_p50', 'frame_p99', 'submit_p50', 'submit_p99', 'gpu_sum_p50', 'gpu_sum_p99',
    'fwd_p50', 'fwd_p99', 'ssr_p50', 'dp_p50', 'tm_p50', 'execute_p50', 'declare_p50',
]
HOLD_METRICS = ['gpu_sum_p50', 'frame_p50', 'submit_p50', 'ssr_p50']  # pool_sessions.py's pooled set
# name pooled_nonhold() prints it under -> the runs.csv column (perf_* = the non-hold --perf p50s)
NONHOLD_METRICS = [('frame_p50', 'perf_frame_p50'), ('submit_p50', 'perf_submit_p50'),
                    ('render_p50', 'perf_render_p50')]

CSV_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'runs.csv')


def load_runs():
    by_session = defaultdict(list)
    with open(CSV_PATH, newline='') as f:
        for row in csv.DictReader(f):
            by_session[row['session']].append(row)
    for s in by_session:
        by_session[s].sort(key=lambda r: int(r['position']))
    return by_session


def adjacent_pairs(rows, key, rounded=False):
    """Same pairing as analyze_unfiltered.py/pool_sessions.py: adjacent runs in session order,
    same-arm pairs go to the A/A noise floor, cross-arm pairs go to A/B signed T-minus-F.

    `rounded` replicates a quirk of the original pool_sessions.py: it pools by spawning
    `analyze_unfiltered.py` as a subprocess and regex-parsing ITS PRINTED, 4-decimal-rounded A/B and
    A/A lists (`eval(m.group(2))`/`eval(m.group(3))` on the list-literal text) rather than re-deriving
    full-precision differences — so the pooled medians/ratios in pooled-analysis.txt are computed
    over already-rounded numbers, not the full-precision deltas the per-session lines show. `pool.py`
    has no subprocess text to reparse, so this reproduces the same rounding directly: round(x, 4) is
    the double nearest that 4-decimal value, and it round-trips through repr()/eval() unchanged, so
    this matches what the original's regex-then-eval path actually consumed, bit for bit."""
    ab, aa = [], []
    for a, b in zip(rows, rows[1:]):
        dlt = float(b[key]) - float(a[key])
        if rounded:
            dlt = round(dlt, 4)
        if a['arm'] == b['arm']:
            aa.append(dlt)
        else:
            ab.append(dlt if b['arm'] == 'T' else -dlt)
    return ab, aa


def print_primary_estimator(by_session):
    for s in SESSIONS:
        rows = by_session[s]
        print(f'=== {s}: PRIMARY ESTIMATOR (adjacent-pair differences; A/B = T-F signed; '
              f'A/A = later-earlier same-arm) — matches `analyze_unfiltered.py {s} 100` ===')
        for k in HOLD_STATS:
            ab, aa = adjacent_pairs(rows, k)
            print(f"  {k:12s} A/B: {[round(x,4) for x in ab]}  median={st.median(ab):+.4f}   "
                  f"A/A (noise floor): {[round(x,4) for x in aa]}  median={st.median(aa):+.4f}")
        print()


def pooled_hold(by_session):
    pooled_ab = {m: [] for m in HOLD_METRICS}
    pooled_aa = {m: [] for m in HOLD_METRICS}
    for s in SESSIONS:
        rows = by_session[s]
        for k in HOLD_METRICS:
            ab, aa = adjacent_pairs(rows, k, rounded=True)
            pooled_ab[k] += ab
            pooled_aa[k] += aa
    print('=== Pooled HOLD-LOOP estimator (unfiltered, primary) — h1+h2+r2 ===')
    print(f"{'metric':14s} {'n_ab':>5s} {'pooled effect':>14s} {'n_aa':>5s} {'pooled |noise|':>15s} {'ratio':>8s}")
    for k in HOLD_METRICS:
        ab, aa = pooled_ab[k], pooled_aa[k]
        eff = st.median(ab)
        noise = st.median([abs(x) for x in aa])
        ratio = eff / noise if noise else float('inf')
        print(f"{k:14s} {len(ab):5d} {eff:+14.4f} {len(aa):5d} {noise:15.4f} {ratio:7.1f}x")


def pooled_nonhold(by_session):
    pooled_ab = {k: [] for k, _ in NONHOLD_METRICS}
    pooled_aa = {k: [] for k, _ in NONHOLD_METRICS}
    for s in SESSIONS:
        rows = by_session[s]
        for k, col in NONHOLD_METRICS:
            # Unlike pooled_hold(), pool_sessions.py's pooled_nonhold() reads each run's .json
            # directly (no subprocess/regex round-trip), so these pairs are NOT pre-rounded.
            ab, aa = adjacent_pairs(rows, col)
            pooled_ab[k] += ab
            pooled_aa[k] += aa
    print('\n=== Pooled NON-HOLD --perf estimator (sim included) — h1+h2+r2 ===')
    print(f"{'metric':14s} {'n_ab':>5s} {'pooled effect':>14s} {'n_aa':>5s} {'pooled |noise|':>15s} {'ratio':>8s}")
    for k, _ in NONHOLD_METRICS:
        ab, aa = pooled_ab[k], pooled_aa[k]
        eff = st.median(ab)
        noise = st.median([abs(x) for x in aa])
        ratio = eff / noise if noise else float('inf')
        print(f"{k:14s} {len(ab):5d} {eff:+14.4f} {len(aa):5d} {noise:15.4f} {ratio:7.2f}x")


def pooled_hold_full_precision(by_session):
    # This is the section ADR-0041's pooled table cites. pooled_hold() above reproduces
    # pool_sessions.py byte-for-byte, rounding included; this recomputes the same pairs at full
    # precision. It changes exactly one cell versus pooled_hold(): ssr_p50's ratio (48.0x here vs
    # 49.1x rounded), because that ratio's noise-floor denominator is small enough for the fourth
    # decimal place to move it — see adjacent_pairs()'s docstring for why the rounded path rounds
    # at all.
    pooled_ab = {m: [] for m in HOLD_METRICS}
    pooled_aa = {m: [] for m in HOLD_METRICS}
    for s in SESSIONS:
        rows = by_session[s]
        for k in HOLD_METRICS:
            ab, aa = adjacent_pairs(rows, k)  # rounded=False (default): full precision
            pooled_ab[k] += ab
            pooled_aa[k] += aa
    print('\n=== Pooled HOLD-LOOP estimator at FULL precision (no per-pair rounding) — h1+h2+r2 ===')
    print(f"{'metric':14s} {'n_ab':>5s} {'pooled effect':>14s} {'n_aa':>5s} {'pooled |noise|':>15s} {'ratio':>8s}")
    for k in HOLD_METRICS:
        ab, aa = pooled_ab[k], pooled_aa[k]
        eff = st.median(ab)
        noise = st.median([abs(x) for x in aa])
        ratio = eff / noise if noise else float('inf')
        print(f"{k:14s} {len(ab):5d} {eff:+14.4f} {len(aa):5d} {noise:15.4f} {ratio:7.1f}x")


def main():
    by_session = load_runs()
    print_primary_estimator(by_session)
    pooled_hold(by_session)
    pooled_nonhold(by_session)
    pooled_hold_full_precision(by_session)


if __name__ == '__main__':
    main()
