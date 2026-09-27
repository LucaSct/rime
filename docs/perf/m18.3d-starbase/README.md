# m18.3d on starbase — the GTX 1060 runs the GPU-driven path, and what pinning cost us

Companion to [`../m18.3d-vg-sweep/README.md`](../m18.3d-vg-sweep/README.md), which is the same sweep
on the workstation's RTX 3060. [ADR-0047](../../adr/0047-two-machines-and-the-starbase-tier.md) gives
starbase its own ratified budget because it is the machine that *hosts*, and this is the first
measurement from it.

Taken inside CT 122 `rime` on starbase (Proxmox, GTX 1060 6GB, driver 580.178.04), 1280x720, 120
measured frames per row after 12 warmup, at commit `8fce420`.

## The result that was not guaranteed

**`gpu_driven_draw` is true on Pascal.** M18's virtual-geometry path needs `multiDrawIndirect` **and**
`shaderDrawParameters` (`engine/rhi/include/rime/rhi/types.hpp`), and the RHI additionally refuses any
device below Vulkan 1.3 with `dynamicRendering` and `synchronization2`. The container reports
`apiVersion 1.4.312` and all four features present, and the sweep then ran every row with the full
framebuffer covered and `frames_without_raster = 0`.

This was the open question in ADR-0047, and it mattered: had it come back false, the sample would have
refused with a counter and **M18's geometry path would have been out of reach on the machine that
hosts** — a cap on the whole tier rather than a detail.

## Measured, GTX 1060 6GB, 720p

| level | candidates | triangles | sel p50 | sub p50 | sub p99 | gpu p50 | gpu p99 | drawn |
|---|---|---|---|---|---|---|---|---|
| depth 1 | 5 | 256 | 0.477 | 0.500 | 0.747 | 0.099 | 0.133 | 4 |
| depth 2 | 17 | 1024 | 0.378 | 0.472 | 0.557 | 0.089 | 0.134 | 16 |
| depth 3 | 65 | 4096 | 0.383 | 0.585 | 0.669 | 0.119 | 0.145 | 64 |
| depth 4 | 257 | 16384 | 0.417 | 1.036 | 1.118 | 0.218 | 0.240 | 256 |
| depth 5 | 1025 | 65536 | 0.523 | 3.162 | 3.728 | 0.529 | 0.562 | 1024 |
| depth 3, capacity 16 | 65 | 4096 | 0.377 | 0.566 | 0.856 | 0.099 | 0.128 | **1** |

## Against the workstation, at the same resolution

Both at 720p, 120 frames, so the comparison is like-for-like in everything except the machine:

| | RTX 3060 | GTX 1060 | ratio |
|---|---|---|---|
| depth 5 `gpu` p50 | 0.274 | 0.529 | **1.9x slower** |
| depth 5 `sub` p50 | 1.949 | 3.162 | **1.6x slower** |
| depth 1 `sel` p50 | 0.695 | 0.477 | **1.5x FASTER** |

Both sides of that table are **unpinned** — the workstation's 720p series was taken before its clocks
were pinned, and starbase's could not be (below) — so the comparison is at least consistent in that
respect, and neither column is a baseline.

The first two are the expected direction and finally put a number on "roughly 2-3x slower", which
ADR-0047 had to infer. **The third is not expected and is not explained here.** `sel` is the
selection dispatch's own blocking submit — a fixed round trip that does no scalable work — and it is
consistently faster on the weaker GPU across every row. Two candidates, neither verified: the drivers
differ (580.178.04 against 610.57.04), and the workstation runs with validation layers enabled in the
dev build while this run is Release inside a container. It is recorded as an anomaly rather than a
finding, because a difference nobody has explained is not evidence for anything.

## The pinning problem, and why this is NOT a ratified baseline

ADR-0047 §2 says the starbase bar is ratified from the first **clock-pinned** run. This run is not
pinned, and the attempt is worth recording because it may not be possible at all:

- `nvidia-smi -lgc 1911,1911` and `-lmc 4004` both reported "All done" on the host.
- `nvidia-smi -ac 4004,1911` likewise.
- The clock nevertheless read **139 MHz at 0% utilisation** for the whole sampling window, and the
  run was taken with `RIME_PERF_ALLOW_UNPINNED_CLOCKS=1` because `perf.sh`'s guard correctly refused
  it otherwise.

The likely reason is that Pascal's application clocks apply to **CUDA** contexts, and this workload is
Vulkan — so the lock is accepted and then never engaged. Locked graphics clocks (`-lgc`) are a
Volta-and-later facility. **If that is right, a clock-pinned Vulkan measurement on this GPU is not
achievable with `nvidia-smi`**, and ADR-0047's precondition cannot be met as written.

That leaves a decision that is the owner's, not this file's: **ratify the starbase bar from unpinned
runs** (accepting wider variance, and saying so wherever the number is quoted), or **have no starbase
bar** and make no hosted performance claim. Nothing here assumes either. The report beside this file
is committed so the numbers exist and are diffable, and it is labelled unpinned in exactly the way the
workstation's first sweep was.

A second, smaller constraint for whoever takes that decision: the container **cannot** pin clocks even
if Pascal supported it, because `nvidia-smi -lgc` needs the host. So a pinned starbase run would always
be a host action wrapped around a container measurement.

## What the tier is bounded by, beyond time

- **6 GB of VRAM**, half the workstation's. `docs/design/hosted-rime.md` analysed concurrency against
  12 GB and called unpartitioned VRAM the thing that kills the box; that analysis needs re-reading at
  half the budget before any concurrency claim.
- **8 cores and 19 GB of RAM on the host**, shared with 13 containers. CT 122 gets 6 cores / 8 GB, and
  `perf.sh`'s contention watcher is what decides whether a given run was fit to measure.
