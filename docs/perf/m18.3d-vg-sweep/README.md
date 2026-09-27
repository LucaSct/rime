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

> **UNPINNED CLOCKS.** These were taken with `RIME_PERF_ALLOW_UNPINNED_CLOCKS=1`; the box reported
> 337 MHz of 2100 MHz graphics and 405 MHz of 7501 MHz memory at the time. They are **not**
> comparable against a pinned baseline, and no report from this run is committed beside them. The
> ratios below are the finding; the absolute milliseconds are not yet a baseline.

| level | groups | leaves | candidates | triangles | sel p50 | sub p50 | sub p99 | gpu p50 | gpu p99 | drawn |
|---|---|---|---|---|---|---|---|---|---|---|
| depth 1 | 5 | 4 | 5 | 256 | 1.180 | 0.536 | 1.265 | 0.110 | 0.121 | 4 |
| depth 2 | 21 | 16 | 17 | 1024 | 1.136 | 0.562 | 0.938 | 0.125 | 0.131 | 16 |
| depth 3 | 85 | 64 | 65 | 4096 | 1.141 | 0.646 | 0.873 | 0.156 | 0.163 | 64 |
| depth 4 | 341 | 256 | 257 | 16384 | 1.142 | 0.958 | 1.482 | 0.225 | 0.244 | 256 |
| depth 5 | 1365 | 1024 | 1025 | 65536 | 1.142 | 2.144 | 2.920 | 0.391 | 0.451 | 1024 |
| depth 3, capacity 16 | 85 | 64 | 65 | 4096 | 1.133 | 0.595 | 1.085 | 0.104 | 0.111 | **1** |

120 measured frames per row after 12 warmup frames. Every row reported `frames_without_raster = 0`
and `covered_pixels = 2073600`, i.e. the full framebuffer.

`sel` is the selection dispatch's own blocking submit; `sub` is gate 4's **CPU submission** (graph
declare + record + submit + wait); `gpu` is the summed `vg-*` pass timestamps. Milliseconds. The
candidate-array build is omitted from the table because it never exceeded 0.0015 ms.

## What it says

**The draw-list machinery scales; the per-frame geometry upload is what costs.** Across a 256x
growth in the cut, CPU submission grows 4.0x (0.536 -> 2.144 ms) and GPU time 3.6x (0.110 -> 0.391
ms) — both strongly sub-linear. But at the top of the sweep **CPU submission costs 5.5x the GPU time
it is feeding**, and that is the number to carry forward.

It is also a cost M18.3c predicted in prose and nobody had measured. `virtual_geometry_visibility_-
pass.hpp` states it plainly: the CPU uploads the geometry of *every candidate*, including coarse
clusters it will usually not draw, "because it no longer knows which ones win… that is the price of
not reading the verdict back, and it goes away with the GPU page pool (M18 step 5), not before."
This sweep is the first measurement of that price. **Step 5 is where it goes, and this table is the
before-picture.**

**Selection's blocking submit is a fixed cost, and at small cuts it is the whole cost.** `sel` moves
1.180 -> 1.142 ms — it does not move at all, and if anything drifts *down* — while the group count
grows 273x (5 -> 1365). It is a submit round trip,
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
readback moved to the last warmup frame. The p50s did not move, which is what says the contamination
was one frame rather than a bias.

A fourth, smaller one: `VirtualGeometryVisibilityStats` is the **pass's** running total, not one
`declare()`'s, so a level's candidate count is a difference. Reading it directly reported 220
candidates for a four-leaf quadtree — forty frames of five, which looks like a plausible number and
is not one. The sample now differences it per frame and cross-checks it against what the CPU
uploaded, failing the row if the two disagree.

## Still open on gate 4

- **A pinned-clock run.** The table above is unpinned and therefore establishes no baseline. Pin
  both domains (`nvidia-smi -pm 1; -lgc 1785,1785; -lmc 7501`) and re-run with `--commit`.
- A pre-existing RHI warning fires once per frame on this path and is not this brick's:
  `initial_data ignored for device-local buffer 'vg-draw-records' / 'vg-indirect-commands' (needs
  staging)`. Both buffers are GPU-written before they are read, so it is noise rather than a bug —
  but it is noise that would hide a real one, and it arrived with M18.3c's move to `GpuOnly` memory.
