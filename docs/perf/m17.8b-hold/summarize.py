#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Summarize the m17.8b hold-loop A/B harness (h1/h2/r2, 8 runs each) into one small runs.csv: one
row per run, with every per-run statistic analyze_unfiltered.py and pool_sessions.py consume. The
per-run math here is copied verbatim from analyze_unfiltered.py's row-building loop (see the copy
of that script alongside this one) so that pool.py, working from runs.csv alone, reproduces its
output exactly. `100` is the settling-frame skip analyze_unfiltered.py is invoked with in the ADR
(`analyze_unfiltered.py <session> 100`): the first 100 rows of each *.hold.csv are warm-up/collapse
frames dropped before any statistic is taken.

Usage: python3 summarize.py [harness-dir]   (default: ~/rime-perf-harness-m17.8b)
Writes runs.csv next to this script.
"""
import csv
import datetime
import glob
import json
import os
import sys
import statistics as st

SESSIONS = ['h1', 'h2', 'r2']
SKIP = 100  # settling-frame skip — see module docstring

# The 13 hold-loop per-run statistics analyze_unfiltered.py computes (its `stats` list), in its order.
HOLD_STATS = [
    'frame_p50', 'frame_p99', 'submit_p50', 'submit_p99', 'gpu_sum_p50', 'gpu_sum_p99',
    'fwd_p50', 'fwd_p99', 'ssr_p50', 'dp_p50', 'tm_p50', 'execute_p50', 'declare_p50',
]
# Non-hold `--perf` per-run p50s pool_sessions.py's pooled_nonhold() reads from each run's .json,
# under distributions[<zone>]['p50_ms']. Prefixed perf_ so they never collide with the hold columns
# above (both loops produce a "frame_p50"/"submit_p50", from different sources).
PERF_ZONES = [
    ('perf_frame_p50', 'frame'),
    ('perf_submit_p50', 'frame.submit'),
    ('perf_render_p50', 'frame.render'),
]


def parse_clk(path):
    out = []
    for line in open(path):
        p = [x.strip() for x in line.split(',')]
        if len(p) < 8:
            continue
        try:
            ts = datetime.datetime.strptime(p[0], '%Y/%m/%d %H:%M:%S.%f').timestamp()
            out.append((ts, int(p[1]), int(p[2]), int(p[3])))
        except ValueError:
            pass
    return out


def pass_series(rows, name):
    out = []
    for r in rows:
        for kv in r['passes'].split(';'):
            if kv.startswith(name + '='):
                out.append(float(kv.split('=')[1]))
                break
        else:
            out.append(float('nan'))
    return out


def med(x):
    return st.median(x)


def q(x, p):
    s = sorted(x)
    return s[min(len(s) - 1, int(p * len(s)))]


def summarize_session(session_dir, session):
    """One row per r*.hold.csv in the session directory, in the same sorted-glob order
    analyze_unfiltered.py iterates (which is also each run's position in the session's T/F order)."""
    rows_out = []
    csv_paths = sorted(glob.glob(os.path.join(session_dir, 'r*.hold.csv')))
    for position, csvp in enumerate(csv_paths, start=1):
        base = csvp[:-len('.hold.csv')]
        name = os.path.basename(base)
        arm = name.split('-')[-1]
        rows_all = list(csv.DictReader(open(csvp)))
        rows = rows_all[SKIP:]  # settling only — no clock filter, matches the primary estimator
        rep = json.load(open(base + '.json'))
        led = rep['ledger']

        clk_all = parse_clk(base + '.clk')
        boosted_n = sum(1 for c in clk_all if c[1] >= 1700 and c[2] >= 7000)
        pct_boosted = boosted_n / len(clk_all) if clk_all else float('nan')

        cpu = [int(l.split()[1]) for l in open(base + '.cpu') if len(l.split()) == 3]

        def col(k):
            return [float(r[k]) for r in rows]

        fp = pass_series(rows, 'forward-pbr shadowed')
        ssr = pass_series(rows, 'ssr-resolve')
        dp = pass_series(rows, 'depth-prepass')
        tm = pass_series(rows, 'tonemap')

        row = dict(
            session=session, run=name, arm=arm, position=position,
            bound=led.get('ground.materials_bound'), draws=led.get('draws.submitted'),
            parts=led.get('parts.alive_end'), n=len(rows), pct_boosted=round(pct_boosted, 4),
            foreign_max=max(cpu) if cpu else None,
            frame_p50=med(col('frame_ms')), frame_p99=q(col('frame_ms'), 0.99),
            submit_p50=med(col('submit')), submit_p99=q(col('submit'), 0.99),
            gpu_sum_p50=med(col('passes_sum')), gpu_sum_p99=q(col('passes_sum'), 0.99),
            fwd_p50=med(fp), fwd_p99=q(fp, 0.99),
            ssr_p50=med(ssr), dp_p50=med(dp), tm_p50=med(tm),
            execute_p50=med(col('execute')), declare_p50=med(col('declare')),
        )
        dist = rep['distributions']
        for out_key, zone in PERF_ZONES:
            row[out_key] = dist[zone]['p50_ms']
        rows_out.append(row)
    return rows_out


def main():
    harness_dir = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else '~/rime-perf-harness-m17.8b')
    all_rows = []
    for session in SESSIONS:
        all_rows += summarize_session(os.path.join(harness_dir, session), session)

    header = (
        ['session', 'run', 'arm', 'position', 'bound', 'draws', 'parts', 'n', 'pct_boosted', 'foreign_max']
        + HOLD_STATS
        + [k for k, _ in PERF_ZONES]
    )
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'runs.csv')
    with open(out_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(header)
        for r in all_rows:
            # repr(), not str(), on every float: Python's repr is the shortest string that round-trips
            # to the exact same double, which is what lets pool.py reproduce bit-identical medians
            # without re-reading the raw per-frame CSVs this script is meant to replace.
            w.writerow([repr(r[h]) if isinstance(r[h], float) else r[h] for h in header])
    print(f"wrote {out_path} ({len(all_rows)} rows)")


if __name__ == '__main__':
    main()
