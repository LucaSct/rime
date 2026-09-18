#!/usr/bin/env python3
"""Summarise a .clk trace: per-second gr/mem/util, plus min/max over the window."""
import sys
rows=[]
for line in open(sys.argv[1]):
    p=[x.strip() for x in line.split(',')]
    if len(p)<8: continue
    try: rows.append((p[0], int(p[1]), int(p[2]), int(p[3]), int(p[4]), float(p[5]), p[6]))
    except ValueError: continue
n=len(rows)
print(f"{n} samples ({n*0.2:.1f}s). per-second: gr/mem MHz util% pstate")
for i in range(0,n,5):
    r=rows[i]; print(f"  t={i*0.2:5.1f}s gr={r[1]:4d} mem={r[2]:4d} util={r[3]:3d} memutil={r[4]:3d} {r[6]} {r[5]:5.1f}W")
if n:
    grs=[r[1] for r in rows]; mems=[r[2] for r in rows]; u=[r[3] for r in rows]
    print(f"  gr min/med/max {min(grs)}/{sorted(grs)[n//2]}/{max(grs)}  mem min/med/max {min(mems)}/{sorted(mems)[n//2]}/{max(mems)}  util med {sorted(u)[n//2]}")
    print(f"  fraction of samples with mem==7501: {sum(1 for m in mems if m>=7000)/n:.2f}, gr>=1700: {sum(1 for g in grs if g>=1700)/n:.2f}")
