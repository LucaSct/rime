#!/usr/bin/env python3
"""Pool the adjacent-pair T/F and A/A differences across h1, h2, r2 (all three interleaved series
collected for m17.8b's re-take), for the hold-loop (unfiltered, primary estimator) metrics AND for
the standard non-hold `frame`/`frame.submit`/`frame.render` distributions from each run's .json.
Reproduces every pooled number cited in ADR-0041's 2026-09-17 amendment. Run from this directory:
    python3 pool_sessions.py
"""
import subprocess, re, glob, json, statistics as st

SESSIONS = ['h1', 'h2', 'r2']
HOLD_METRICS = ['gpu_sum_p50', 'frame_p50', 'submit_p50', 'ssr_p50']


def pooled_hold():
    pooled_ab = {m: [] for m in HOLD_METRICS}
    pooled_aa = {m: [] for m in HOLD_METRICS}
    for s in SESSIONS:
        out = subprocess.run(['python3', 'analyze_unfiltered.py', s, '100'],
                              capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r'\s*(\S+)\s+A/B:\s*(\[[^\]]*\])\s+median=\S+\s+'
                          r'A/A \(noise floor\):\s*(\[[^\]]*\])', line)
            if m and m.group(1) in HOLD_METRICS:
                pooled_ab[m.group(1)] += eval(m.group(2))
                pooled_aa[m.group(1)] += eval(m.group(3))
    print('=== Pooled HOLD-LOOP estimator (unfiltered, primary) — h1+h2+r2 ===')
    print(f"{'metric':14s} {'n_ab':>5s} {'pooled effect':>14s} {'n_aa':>5s} {'pooled |noise|':>15s} {'ratio':>8s}")
    for k in HOLD_METRICS:
        ab, aa = pooled_ab[k], pooled_aa[k]
        eff = st.median(ab)
        noise = st.median([abs(x) for x in aa])
        ratio = eff / noise if noise else float('inf')
        print(f"{k:14s} {len(ab):5d} {eff:+14.4f} {len(aa):5d} {noise:15.4f} {ratio:7.1f}x")


def pooled_nonhold():
    metrics = {
        'frame_p50': ('frame', 'p50_ms'),
        'submit_p50': ('frame.submit', 'p50_ms'),
        'render_p50': ('frame.render', 'p50_ms'),
    }
    pooled_ab = {k: [] for k in metrics}
    pooled_aa = {k: [] for k in metrics}
    for s in SESSIONS:
        rows = []
        for f in sorted(glob.glob(f'{s}/r*.json')):
            j = json.load(open(f))
            arm = 'T' if f.split('/')[-1].split('-')[-1].startswith('T') else 'F'
            dist = j['distributions']
            row = {'arm': arm}
            for k, (zone, field) in metrics.items():
                row[k] = dist[zone][field]
            rows.append(row)
        for k in metrics:
            ab, aa = [], []
            for a, b in zip(rows, rows[1:]):
                dlt = b[k] - a[k]
                if a['arm'] == b['arm']:
                    aa.append(dlt)
                else:
                    ab.append(dlt if b['arm'] == 'T' else -dlt)
            pooled_ab[k] += ab
            pooled_aa[k] += aa
    print('\n=== Pooled NON-HOLD --perf estimator (sim included) — h1+h2+r2 ===')
    print(f"{'metric':14s} {'n_ab':>5s} {'pooled effect':>14s} {'n_aa':>5s} {'pooled |noise|':>15s} {'ratio':>8s}")
    for k in metrics:
        ab, aa = pooled_ab[k], pooled_aa[k]
        eff = st.median(ab)
        noise = st.median([abs(x) for x in aa])
        ratio = eff / noise if noise else float('inf')
        print(f"{k:14s} {len(ab):5d} {eff:+14.4f} {len(aa):5d} {noise:15.4f} {ratio:7.2f}x")


if __name__ == '__main__':
    pooled_hold()
    pooled_nonhold()
