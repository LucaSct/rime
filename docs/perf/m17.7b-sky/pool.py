#!/usr/bin/env python3
"""Pool m17.7b's sky A/B. Regenerates the table in README.md — do not hand-edit those numbers.

Usage: python3 docs/perf/m17.7b-sky/pool.py
"""
import statistics as st
import pathlib

rows = [l.split() for l in (pathlib.Path(__file__).parent / "runs.txt").read_text().splitlines() if l.strip()]
data: dict[str, dict[str, list[float]]] = {}
for r in rows:
    arm = r[0]
    for kv in r[2:]:
        k, v = kv.split("=")
        data.setdefault(k, {}).setdefault(arm, []).append(float(v))

print(f"{'metric':<12} {'on median':>10} {'off median':>11} {'delta':>8} "
      f"{'on spread':>10} {'off spread':>11} {'separated':>10}")
for k, d in data.items():
    on, off = sorted(d["on"]), sorted(d["off"])
    mon, moff = st.median(on), st.median(off)
    delta = mon - moff
    spread = max(max(on) - min(on), max(off) - min(off))
    # "Separated" means the arms' medians differ by more than the WIDER arm's own run-to-run
    # spread. It is a deliberately blunt test, and it is the whole point of interleaving: on an
    # unpinned box a delta smaller than the noise floor is not a measurement, it is a coincidence.
    print(f"{k:<12} {mon:>10.3f} {moff:>11.3f} {delta:>+8.3f} "
          f"{max(on)-min(on):>10.3f} {max(off)-min(off):>11.3f} "
          f"{'yes' if abs(delta) > spread else 'NO':>10}")
