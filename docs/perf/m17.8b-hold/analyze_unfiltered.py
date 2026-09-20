#!/usr/bin/env python3
"""Primary estimator per QWEN-ESTIMATOR-VERDICT.md: unfiltered per-run median over all post-settling
hold frames, adjacent-pair T-F differences as the effect, same-arm adjacent pairs as the empirical
noise floor. The .clk trace is read only for a per-run diagnostic (pct_boosted), never as a filter.
Also reports the OLD filtered estimator (analyze_hold.py's clock-boost gate) side by side, as the
sensitivity check the verdict asks for — filtered is not the primary method here.
"""
import csv, glob, os, sys, json, statistics as st, datetime, bisect

d = sys.argv[1]
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 100


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


runs = []
for csvp in sorted(glob.glob(os.path.join(d, 'r*.hold.csv'))):
    base = csvp[:-9]
    name = os.path.basename(base)
    arm = name.split('-')[-1]
    rows_all = list(csv.DictReader(open(csvp)))
    rows = rows_all[skip:]  # settling only — NO clock filter on the primary estimator
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
        run=name, arm=arm, bound=led.get('ground.materials_bound'),
        draws=led.get('draws.submitted'), parts=led.get('parts.alive_end'),
        n=len(rows), pct_boosted=round(pct_boosted, 4),
        foreign_max=max(cpu) if cpu else None,
        frame_p50=med(col('frame_ms')), frame_p99=q(col('frame_ms'), 0.99),
        submit_p50=med(col('submit')), submit_p99=q(col('submit'), 0.99),
        gpu_sum_p50=med(col('passes_sum')), gpu_sum_p99=q(col('passes_sum'), 0.99),
        fwd_p50=med(fp), fwd_p99=q(fp, 0.99),
        ssr_p50=med(ssr), dp_p50=med(dp), tm_p50=med(tm),
        execute_p50=med(col('execute')), declare_p50=med(col('declare')),
    )
    runs.append(row)

hdr = list(runs[0].keys())
print('=== per-run summary (UNFILTERED, all', len(runs[0].keys()) and (len(rows_all) - skip) if False else '', 'post-settling frames) ===')
print(' | '.join(hdr))
for r in runs:
    print(' | '.join(f"{r[h]:.4f}" if isinstance(r[h], float) else str(r[h]) for h in hdr))

# Clock diagnostic — flag any run under 50% boosted, per the verdict's instruction (report, don't
# filter).
print('\n=== clock diagnostic (pct of .clk samples with gr>=1700 AND mem>=7000) ===')
for r in runs:
    flag = ' <-- FLAG (<50% boosted)' if r['pct_boosted'] < 0.5 else ''
    print(f"  {r['run']:10s} arm={r['arm']} pct_boosted={r['pct_boosted']:.3f} foreign_max={r['foreign_max']}{flag}")

stats = ['frame_p50', 'frame_p99', 'submit_p50', 'submit_p99', 'gpu_sum_p50', 'gpu_sum_p99',
          'fwd_p50', 'fwd_p99', 'ssr_p50', 'dp_p50', 'tm_p50', 'execute_p50', 'declare_p50']

print('\n=== per-arm median [min,max] (UNFILTERED) ===')
T = [r for r in runs if r['arm'] == 'T']
F = [r for r in runs if r['arm'] == 'F']
for k in stats:
    tv = [r[k] for r in T]
    fv = [r[k] for r in F]
    dis = 'Y' if (min(tv) > max(fv) or max(tv) < min(fv)) else 'n'
    print(f"  {k:12s} T {st.median(tv):7.4f} [{min(tv):.4f},{max(tv):.4f}]  F {st.median(fv):7.4f} "
          f"[{min(fv):.4f},{max(fv):.4f}]  d {st.median(tv)-st.median(fv):+.4f} "
          f"({100*(st.median(tv)-st.median(fv))/st.median(fv):+.2f}%) disjoint={dis}")

print('\n=== PRIMARY ESTIMATOR: adjacent-pair differences (A/B = T-F signed; A/A = later-earlier same-arm) ===')
ab_by_stat = {}
aa_by_stat = {}
for k in stats:
    ab = []
    aa = []
    for a, b in zip(runs, runs[1:]):
        dlt = b[k] - a[k]
        if a['arm'] == b['arm']:
            aa.append(dlt)
        else:
            ab.append(dlt if b['arm'] == 'T' else -dlt)
    ab_by_stat[k] = ab
    aa_by_stat[k] = aa
    print(f"  {k:12s} A/B: {[round(x,4) for x in ab]}  median={st.median(ab):+.4f}   "
          f"A/A (noise floor): {[round(x,4) for x in aa]}  median={st.median(aa):+.4f}")

print('\n=== Effect estimate (median A/B pair diff) vs noise floor (median |A/A| pair diff) ===')
for k in stats:
    ab = ab_by_stat[k]
    aa = aa_by_stat[k]
    eff = st.median(ab)
    noise = st.median([abs(x) for x in aa]) if aa else float('nan')
    print(f"  {k:12s} effect={eff:+.4f} ms   |A/A| noise floor median={noise:.4f} ms   "
          f"n_ab={len(ab)} n_aa={len(aa)}")
