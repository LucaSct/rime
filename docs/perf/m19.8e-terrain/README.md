# m19.8e — what streamed terrain costs, and how fast you can fly over it

**UNPINNED. Every number on this page was taken with the GPU clocks free to move.** Read it as the
shape of the costs and the order of the envelope, not as a baseline. No JSON from these runs is
filed in `docs/perf/`; the owner will re-run pinned (see the end).

The measurement is `samples/15-terrain` ([ADR-0073](../../adr/0073-m19.8e-terrain-budgets.md)):
a straight, one-way fly-through of a generated 4 km world cooked by `rime terrain-world`, at walk
(5 m/s), vehicle (30) and aircraft (150) speed, plus 300–4800 m/s for the envelope, paced to 60 Hz
and warm-started, with the byte budget, upload cap and frustum culling on.

```bash
scripts/build.sh --preset release --cpp-only --no-tests
samples/15-terrain/make_world.sh                 # once: ~3 s, 1365 tiles, 30 MB in build/
scripts/perf.sh --sample terrain --frames 600    # RTX 3060; adds --envelope
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json \
    build/release/bin/terrain_flythrough --perf --envelope --frames 600 [--cap-kib 1024]
```

## What was and was not pinned

- **Not pinned.** Pinning needs `sudo nvidia-smi -lgc/-lmc`; this session had no non-interactive
  sudo, and the brick's rule is not to bypass that. `perf.sh`'s in-run clock sampler measured the
  RTX 3060's graphics clock at **412–1942 MHz (median 697, 159–235 % spread)** and memory at
  405–7501 MHz — `stable=false`, so `perf.sh` refused to file the report, as it should. RADV has no
  clock sampler here at all.
- **Why the clock moves so much here.** The loop is paced to 60 Hz and terrain is ~3 ms of GPU per
  frame, so the card is idle most of each frame and the governor parks it. The GPU columns are
  therefore pessimistic and noisy; the CPU columns (selection, residency) and every count are not
  clock-dependent.
- **What does not depend on the clock at all:** draws, fallback and culled leaves, cap waits, upload
  and resident bytes. They are identical on the two GPUs, run for run — the residency's policy is
  deterministic, and with the loop on pace the fallback is set by the cap, not by IO timing.
- Raw logs and JSON: `~/.local/share/rime-specs/m19.8e-perf-unpinned/` on the reference machine,
  measured at commit `9c13fe4` + the edits committed as `7dc52d2` (the pacing rule and the default
  cap). The first run (`…-pingpong-DISCARDED.log`) is kept as a record of a broken path: it re-flew
  the same diagonal and measured a warm cache.

## The machine and the world

| | |
|---|---|
| GPUs | NVIDIA GeForce RTX 3060 (NVIDIA 610.57.04); AMD Raphael iGPU (RADV, Mesa 26.2.1) |
| CPU / OS | AMD Ryzen 9 9950X3D · CachyOS, Linux 7.2.0 |
| Build | `release` preset (RelWithDebInfo), no sanitizer, 1920×1080 |
| World | 32×32 level-0 tiles of 65×65 samples at 2 m (4.1 km square), 6 levels, 1365 tiles, 341 baked parents, two terrain layers with height blend |
| LOD | τ = 1 px at 1080p, 60° FOV; μ = one frame's step (speed / 60) |
| Budget | 96 MiB, 2048 slots (the bytes bind; the slots never do) |

**Bytes per slot (exact, `TerrainPass::predicted_tile_bytes`):** a level-0 splat tile is
**108 070 B** — heights 8 450, **indices 98 304**, weights 1 156, uniform block 160; a parent is
**140 718 B** — the same plus its bake, **33 800 B** (8·N²). The per-tile index buffer is 91 % of a
splat tile and 70 % of a parent, and it is identical for every tile: one shared index buffer would
cut terrain GPU memory by about 4×. Layer textures: 2 × 340 B here (8×8 with mips).

## RTX 3060, cap 256 KiB/frame (the default)

| speed | m/s | frames | GPU p50 / p99 ms | selection p50 / p99 ms | residency p50 / p99 ms | upload p50 / max KiB | GPU bytes peak | fallback | on pace | cap waits |
|---|---|---|---|---|---|---|---|---|---|---|
| walk | 5 | 600 | 2.22 / 3.75 | 0.25 / 0.29 | 0.35 / 2.74 | 0 / 243 | 25.1 MiB | 0.00 % | 100 % | 1 777 |
| vehicle | 30 | 600 | 3.25 / 4.38 | 0.30 / 0.35 | 0.43 / 16.5 | 0 / 243 | 31.5 MiB | 0.00 % | 96.7 % | 1 857 |
| aircraft | 150 | 600 | 3.20 / 3.82 | 0.39 / 0.59 | 0.72 / 13.9 | 0 / 243 | 53.7 MiB | 0.00 % | 98.3 % | 2 351 |
| | 300 | 600 | 2.96 / 3.55 | 0.42 / 0.70 | 8.80 / 17.4 | 137 / 243 | 84.1 MiB | 0.11 % | 96.7 % | 5 293 |
| | 600 | 532 | 2.11 / 3.14 | 0.38 / 0.55 | 9.22 / 19.3 | 137 / 243 | 92.2 MiB | **8.65 %** | 97.4 % | 34 296 |
| | 1200 | 266 | 0.88 / 2.71 | 0.34 / 0.48 | 9.57 / 17.3 | 211 / 243 | 67.7 MiB | 36.4 % | 95.1 % | 26 390 |

"Residency" is `begin_frame` whole: retire, reclaim, both selections, requests, palette and tile
**uploads**. "On pace" = frames whose residency + terrain GPU fit 16.67 ms. Fallback is the share
of *drawn* leaves (after culling) that are coarser than the selection asked. Peak GPU bytes per
speed come from the JSON ledgers (`peak_budget_bytes`: tiles resident and retiring, plus layer
textures). At this cap the 96 MiB budget never binds — the peak is 92.2 MiB at 600 m/s — so the
cap, not the budget, sets the envelope; with the 1 MiB cap the budget does bind from 600 m/s up
(peak 96.0 MiB, exactly the budget, and never above it).

**Travel envelope, RTX 3060, 96 MiB, 256 KiB/frame: 300 m/s** (UNPINNED). Walk, vehicle and
aircraft speed are all inside it with no measurable fallback; 600 m/s fails on fallback.

## The cap is the knob, and its right value depends on the machine

| | cap | envelope | what limits it |
|---|---|---|---|
| RTX 3060 | 256 KiB/frame | **300 m/s** | fallback (8.7 % at 600) |
| RTX 3060 | 1 MiB/frame | **30 m/s** | **pace**: 89.8 % at 150 m/s, residency p99 43 ms |
| RADV iGPU | 256 KiB/frame | **300 m/s** | fallback — the same counts as the 3060, to the leaf |
| RADV iGPU | 1 MiB/frame | **2400 m/s** | fallback (20.5 % at 4800); residency p99 ≤ 2.6 ms throughout |

**Measured:** on the 3060 a frame that uploads ~1 MiB spends 30–60 ms in `begin_frame`; on RADV the
same bytes cost ≤ 2.6 ms. **Inferred, not measured:** `TerrainPass::upload` is synchronous — each
tile is several blocking `write_texture`/`write_buffer` calls — and on a discrete card each one is
a staged copy across PCIe that the frame waits for, where a UMA iGPU writes memory the GPU already
sees. Either way the CPU cost of an upload, not its bandwidth, is what a cap has to bound on a
discrete GPU, and an asynchronous upload path (a staging ring on a transfer queue) is what would
let the 3060 use the 1 MiB cap RADV can.

## RADV, cap 256 KiB/frame

| speed | m/s | GPU p50 / p99 ms | selection p50 / p99 ms | residency p50 / p99 ms | fallback | on pace |
|---|---|---|---|---|---|---|
| walk | 5 | 2.71 / 3.11 | 0.18 / 0.26 | 0.27 / 0.77 | 0.00 % | 100 % |
| vehicle | 30 | 2.90 / 3.55 | 0.22 / 0.33 | 0.34 / 0.88 | 0.00 % | 100 % |
| aircraft | 150 | 4.80 / 5.46 | 0.33 / 0.54 | 0.60 / 1.15 | 0.00 % | 100 % |
| | 300 | 4.55 / 5.12 | 0.38 / 0.67 | 0.84 / 1.34 | 0.11 % | 100 % |
| | 600 | 3.71 / 4.82 | 0.36 / 0.53 | 1.10 / 1.59 | 8.65 % | 100 % |

## What this does not say

- **GPU ms is ~95–120 raster passes**, one per drawn leaf (ADR-0071's known limit); every pass is
  timed (`untimed_frames` = 0 in every ledger). How much of the ~3 ms is per-pass overhead rather
  than shading was not isolated, nor was the 8d3 bake-fetch cost (up to 16 fetches per pixel).
- Uploads are synchronous; the cap bounds how much a frame uploads, not how long one upload stalls.
- One world, one straight path, one τ. A rougher world or τ < 1 px wants more leaves; at the
  first world generated (1.5 m sample-scale roughness) a frame drew ~280 leaves at 360p, past the
  graph's 128 timed passes, and the world was regenerated with realistic 0.15 m roughness.
- Fallback < 1 % is the envelope's criterion; a fallback frame is still covered (coarser, never a
  hole: `uncovered` = 0 in every ledger).

## For the owner: re-running pinned

```bash
sudo nvidia-smi -pm 1 && sudo nvidia-smi -lgc 1785,1785 && sudo nvidia-smi -lmc 7501
scripts/perf.sh --sample terrain --frames 600 --commit     # files the JSON if the clock held
sudo nvidia-smi -rgc && sudo nvidia-smi -rmc
```

`perf.sh --sample terrain` cooks the world on first use and passes `--envelope`. The budgets in
ADR-0073 are to be confirmed from that run.
