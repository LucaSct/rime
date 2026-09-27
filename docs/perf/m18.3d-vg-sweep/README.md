# m18.3d — the virtual-geometry complexity sweep (ADR-0043 gate 4)

Gate 4 has two halves. M18.3b/c built the first: GPU selection plus indirect submission agreeing
with the CPU oracle, with no readback deciding the same frame's draw list. This is the second —
*"complexity sweeps report candidates, selected triangles, CPU submission and GPU time at a fixed
projected size"* — and until this brick there was no virtual-geometry scenario in `scripts/perf.sh`
at all, so there was nothing to run.

Run it with `scripts/perf.sh --sample virtual-geometry`, or the sample directly:

```
build/release/bin/virtual_geometry --sweep --frames 120 --width 1920 --height 1080
```

## What the sweep varies, and what it holds still

The independent variable is the **size of the cut**, and it has to move while everything a number
could otherwise be blamed on holds still. A cooked mesh cannot do that: changing its cut also
changes its silhouette, material count, page layout and residency. So the sample generates a
**quadtree** — one replacement group per node, one cluster per group, one page per cluster, every
page permanently resident — and depth alone decides how many leaves there are.

`pixels_per_metre` and `max_projected_error_px` are **constants** (1.0 and 1.0). Every interior
group's LOD error is above the threshold and every leaf's is below it, so the cut is always "all the
leaves", chosen by the same policy a camera would use. That is what "at a fixed projected size"
buys: if the sweep moved the camera instead, a row's candidate count and its projected error would
change together and neither column would mean anything alone.

Because each node's children tile its square, the leaves of **any** depth cover the viewport exactly
once. That is why `px cov` is the full framebuffer on every row, and it is the GPU column's vacuity
guard: a raster pass that covers no pixels is the cheapest one there is.

## Measured, 2026-09-27, RTX 3060 (NVIDIA 610.57.04), 1920x1080, 64 triangles/cluster

Clock-pinned (`nvidia-smi -pm 1; -lgc 1785,1785; -lmc 7501`), Release/RelWithDebInfo, the box quiet
for the whole run. The committed report beside this file is
[`2026-09-27-14-virtual-geometry-nvidia-geforce-rtx-3060.json`](../2026-09-27-14-virtual-geometry-nvidia-geforce-rtx-3060.json),
and it **establishes** this machine's baseline rather than confirming one — the regression check has
nothing to compare against until a second run exists.

| level | groups | leaves | candidates | triangles | sel p50 | sub p50 | sub p99 | gpu p50 | gpu p99 | drawn |
|---|---|---|---|---|---|---|---|---|---|---|
| depth 1 | 5 | 4 | 5 | 256 | 1.223 | 0.560 | 0.951 | 0.113 | 0.116 | 4 |
| depth 2 | 21 | 16 | 17 | 1024 | 1.227 | 0.626 | 1.493 | 0.135 | 0.139 | 16 |
| depth 3 | 85 | 64 | 65 | 4096 | 1.234 | 0.701 | 1.013 | 0.169 | 0.176 | 64 |
| depth 4 | 341 | 256 | 257 | 16384 | 1.234 | 0.988 | 1.385 | 0.243 | 0.261 | 256 |
| depth 5 | 1365 | 1024 | 1025 | 65536 | 1.243 | 2.166 | 2.999 | 0.418 | 0.478 | 1024 |
| depth 3, capacity 16 | 85 | 64 | 65 | 4096 | 1.233 | 0.734 | 1.001 | 0.113 | 0.119 | **1** |

120 measured frames per row after 12 warmup frames. Every row reported `frames_without_raster = 0`
and `covered_pixels = 2073600`, i.e. the full framebuffer.

`sel` is the selection dispatch's own blocking submit; `sub` is gate 4's **CPU submission** (graph
declare + record + submit + wait); `gpu` is the summed `vg-*` pass timestamps. Milliseconds. The
candidate-array build is omitted from the table because it never exceeded 0.0015 ms.

## What it says

**The draw-list machinery scales; the per-frame geometry upload is what costs.** Across a 256x
growth in the cut, CPU submission grows 3.9x (0.560 -> 2.166 ms) and GPU time 3.7x (0.113 -> 0.418
ms) — both strongly sub-linear. But at the top of the sweep **CPU submission costs 5.2x the GPU time
it is feeding**, and that is the number to carry forward.

It is also a cost M18.3c predicted in prose and nobody had measured. `virtual_geometry_visibility_-
pass.hpp` states it plainly: the CPU uploads the geometry of *every candidate*, including coarse
clusters it will usually not draw, "because it no longer knows which ones win… that is the price of
not reading the verdict back, and it goes away with the GPU page pool (M18 step 5), not before."
This sweep is the first measurement of that price. **Step 5 is where it goes, and this table is the
before-picture.**

**Selection's blocking submit is a fixed cost, and at small cuts it is the whole cost.** `sel` moves
1.223 -> 1.243 ms — 1.6%, which at this spread is not a trend — while the group count grows 273x
(5 -> 1365). It is a submit round trip,
not selection work. At depth 1 it is **2.2x** the CPU submission and **11x** the GPU time. Folding
selection into the frame graph (named as later work in M18.3c) is therefore worth more than any
amount of tuning inside the selector.

**Overflow degrades where it should.** The last row runs the 64-leaf cut against a 16-command
capacity: `selected_total` 64, `emitted` 1, `fell_back_to_coarse` 1, `overflow_without_coarse` 0,
and the viewport stays fully covered because the coarse cluster is the whole quadtree's root. Gate 4
asks for overflow to be counted and to fall back safely; that row is the evidence. Note that depth 5
does **not** overflow — 1024 leaves sit exactly at the 1024-command capacity, and the coarse cluster
costs a *candidate* without costing a command.

## Two harness bugs worth recording, because both produced plausible numbers

**1. The render graph culled the pass being measured.** The first version exported the visibility
target on one frame only (to read pixels back). The graph drops passes whose outputs nothing
consumes, so on every other frame the raster pass never ran — while the compute builder, which
writes an *imported* buffer, survived the cull and kept reporting a time. The sweep reported
`gpu p50` 0.015 ms at 720p **and** 0.015 ms at 1080p, identical to four significant figures, which
is the tell: 6 µs for a 2-megapixel draw is below a 3060's fill rate. The fix exports both targets
every frame; the guard is `frames_without_raster`, which counts any measured frame whose timings
lack the raster pass and fails the run. A culled pass does not report a slow time, it reports **no**
time, and a sum over "whatever came back" cannot tell that from a fast pass.

**2. The work ledger borrows its key strings.** `WorkCounter::name` is a `std::string_view`,
documented as expecting literals. Generated keys written through a reused stack buffer all aliased
the same 64 bytes, so the sweep printed a correct table beside a ledger of fiction
(`"vg.d5.skipped_ov": 45100`). The sample now keeps keys in a `std::deque<std::string>`, which is
what gives stable addresses across growth.

**3. A full-framebuffer readback inside a measured frame's submit window.** The coverage check
copies the visibility target to a host buffer — 16 MB at 1080p, recorded in the same command buffer
the submit timer covers. It originally ran on the *first measured* frame, and nearest-rank p99 of 120
samples **is** the 119th sample, so that one frame could be the `sub p99` being reported. It was: at
depths 2 and 3 the reported `sub p99` fell from 1.215 and 1.273 ms to 0.938 and 0.873 ms once the
readback moved to the last warmup frame. (Both of those runs were unpinned, so they are comparable
with each other and not with the pinned table above, whose p99s are noisier still — `sub p99` is the
one column here where a single frame is the statistic.) The p50s did not move, which is what says the
contamination was one frame rather than a bias.

A fourth, smaller one: `VirtualGeometryVisibilityStats` is the **pass's** running total, not one
`declare()`'s, so a level's candidate count is a difference. Reading it directly reported 220
candidates for a four-leaf quadtree — forty frames of five, which looks like a plausible number and
is not one. The sample now differences it per frame and cross-checks it against what the CPU
uploaded, failing the row if the two disagree.

## A note on pinning, since it costs about 7%

The same sweep taken minutes earlier with `RIME_PERF_ALLOW_UNPINNED_CLOCKS=1` was consistently
*faster*: `sel` 1.180 vs 1.223 ms, depth-5 `sub` 2.144 vs 2.166, depth-5 `gpu` 0.391 vs 0.418. That
is the expected direction and not a contradiction. Pinning parks the graphics clock at **1785 MHz**
against a 2100 MHz boost ceiling, so a pinned run is slower than an unpinned one that happened to
boost — it trades peak throughput for a number that means the same thing tomorrow. The unpinned
figures are recorded here only to document the size of that trade; they are not a baseline and
nothing compares against them.

## Still open on gate 4

- Nothing. Gate 4's two halves — GPU selection plus indirect submission against the oracle
  (M18.3b/c), and this complexity sweep — are both delivered, with overflow counted and degrading to
  the coarse cut. **Gate 6** (the hybrid micro-triangle raster path) is next, and its own clause
  requires "the split earns a measured micro-triangle gain" — which this harness is now what
  measures. Vary `--triangles` against a fixed cut to move triangle *size* rather than count.
- A pre-existing RHI warning fires once per frame on this path and is not this brick's:
  `initial_data ignored for device-local buffer 'vg-draw-records' / 'vg-indirect-commands' (needs
  staging)`. Both buffers are GPU-written before they are read, so it is noise rather than a bug —
  but it is noise that would hide a real one, and it arrived with M18.3c's move to `GpuOnly` memory.
