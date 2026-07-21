# The GI feasibility spike — how much does an SDF sphere-trace cost?

**Status:** measured 2026-07-21 on lavapipe, after m10.4b landed the clipmap.
**Scope:** fixes m10.5's *proof numbers* — probe count, rays per probe, and whether the grid can be
updated every frame. It does **not** move the architecture; [ADR-0032](../adr/0032-lighting-v2.md) §2
already settled that, and said so explicitly when it scheduled this spike.

## The question the ADR refused to guess at

> The **critical unknown is the SDF-trace feasibility spike** — rays/second at probe-grid scale on
> lavapipe. It cannot be answered by reading code; it must be measured.
> — ADR-0032 §2

M10's GI is DDGI probes whose rays are sphere-traced through the global SDF clipmap. A probe grid of
*P* probes each casting *R* rays costs *P × R* traces per update. Nobody knew what a trace costs, so
nobody could say whether *P × R* was 30,000 or 3,000,000.

## How it was measured

`tests/render/sdf_clipmap_test.cpp`, the test case *"gi spike: sphere-trace cost at DDGI probe-grid
scale"*. It lives in the suite rather than in a throwaway harness for two reasons: anyone can
reproduce the numbers by running the tests, and the same sweep re-measures itself for free on real
hardware at m12.0, where it stops being a shape and becomes the actual budget.

The scene is a small room — floor, four walls, three pillars — sized to sit inside clipmap level 0's
own 8 m extent, so every probe and ray stays in the finest, most expensive level. Probes sit on a
lattice through the room, each casting 64 spherical-Fibonacci rays (the DDGI norm) out to 8 m.

Two methodological notes, both of which changed the numbers:

- **Warm up before measuring.** The first dispatch through a fresh compute pipeline pays lavapipe's
  shader JIT. Un-warmed, the smallest grid measured *40× dearer per ray* than the largest — a pure
  artefact that looked exactly like a real fixed-overhead curve.
- **Two clocks.** GPU timestamps bracket the dispatch alone; a CPU wall clock spans the whole
  submit. They bracket each other consistently (wall ≈ GPU + 0.2 ms of buffer create/upload/
  readback). A spike that trusts one clock is how you publish a number that is off by an order of
  magnitude.

## Results (lavapipe, 16 cores; two runs)

| Ray population | Rays | GPU ms | ns/ray |
|---|---:|---:|---:|
| Room, 4³ probes × 64 | 4,096 | 0.082 | 20.0 |
| Room, 6³ probes × 64 | 13,824 | 0.160 | 11.5 |
| Room, 8³ probes × 64 | 32,768 | 0.33 – 0.48 | 10.1 – 14.6 |
| **Empty field** (every ray runs to max_dist) | 32,768 | 0.42 – 0.44 | 12.7 – 13.4 |
| **Grazing** (parallel to a surface, just above it) | 32,768 | 1.44 – 1.45 | 43.9 – 44.3 |

In the furnished room ~20 % of rays escape; among the grazing rays ~21 % exhaust the 128-step cap.

## Three findings

**1. The narrow band puts a floor under the step size, so empty space is cheap.** The intuition
"rays that hit nothing are the expensive ones" is wrong here, and the measurement says so: the empty
field costs the *same* as the furnished room. A sample out in open space reads the saturated band
value and advances a full band — 4 voxels, 0.5 m at level 0 — so an 8 m trace through nothing costs
~16 steps, not 128. The band is not just an encoding compromise; it is a **cost bound**.

**2. Grazing rays are the ceiling, at 3.5×.** What actually burns the step budget is a ray running
parallel to a surface and just above it: the field stays small, every step is small, and the march
hits its iteration cap without converging. This is the number a budget must be built on, and unlike
the other two it is a property of the *trace*, not of how full the scene happens to be — you cannot
author your way out of it.

**3. Cost is linear in ray count once the dispatch is saturated.** 4k rays carry visible fixed
overhead (20 ns/ray); by 14k it is amortized (11.5 ns/ray) and stays there. Probe grids of interest
are all comfortably in the linear region.

## What this means for m10.5

**A full-grid update of 8³ = 512 probes × 64 rays every frame is affordable.** It costs 0.33–0.48 ms
on lavapipe, which is **≈1.0–1.5× the m10.3 cluster cull at 1000 lights** (0.325 ms, measured in the
same suite on the same box). That is the ADR-0032 §11 relative fence, and a technique that costs
about what the light cull costs is not the thing that will sink the frame.

**Beyond ~32k rays per update, amortize.** 16³ = 4096 probes × 64 rays is 262k rays ≈ 3.3 ms
realistic and ~11.6 ms if the population were all grazing — too much for one frame. The standard
DDGI answer applies: cycle the grid, updating 1/8th of the probes per frame, which lands each frame
back on the 512-probe budget above and gives the full grid an 8-frame refresh.

So m10.5's starting configuration:

| Parameter | Value | Why |
|---|---|---|
| Probes | 8³ = 512 per update | the measured full-frame budget |
| Rays / probe | 64 | the DDGI norm, and inside the linear region |
| Max trace distance | 8 m (level 0's extent) | beyond it the coarser levels answer |
| Update policy | full grid if ≤ 512 probes, else round-robin | keeps per-frame cost flat as the grid grows |

## The levers, if m10.5 needs to buy time

- **The band width** (`kSdfClipmapBandVoxels`, currently 4) sets the open-space step directly. Wider
  band ⇒ fewer steps ⇒ cheaper, at the cost of a coarser field near surfaces. This is the biggest
  and most direct lever, and finding 1 is why.
- **The step cap** (128 in `sdf_probe_trace.comp`) bounds the grazing case. Lowering it makes the
  worst case cheaper and turns some grazing near-hits into misses — an accuracy trade, not a
  correctness one, since a DDGI miss just falls back to the sky term.
- **Rays per probe** trades noise for cost linearly, and DDGI's temporal accumulation is designed to
  absorb exactly that.

## What this does not tell us

lavapipe runs compute on the CPU, so these milliseconds are **not** a frame budget — ADR-0032 §11 is
explicit that absolute budgets wait for real hardware at m12.0. What transfers is the *shape*: the
band-sets-the-floor result, the 3.5× grazing ratio, linearity in ray count, and the relative fence
against the cluster cull. Re-run this same test case on a real GPU at m12.0 and the table refills
itself.

It also says nothing about the *rest* of DDGI — irradiance blending, the probe atlas, visibility
(Chebyshev) weighting, or the cost of updating probe textures. Those are m10.5's own work; this
spike bounds only the ray-tracing half, which was the half nobody could estimate.
