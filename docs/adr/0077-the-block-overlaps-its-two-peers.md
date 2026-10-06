# ADR-0077: the block runs its server and client halves at once — and what `sim.block` now means

- Status: **Provisional** — accepted only after Luca's pinned RTX 3060 run (see "What must be checked")
- Date: 2026-10-06

## Context

M18 gate 7 (inherited from M13/M17 through ADR-0043): on the RTX 3060, `scripts/perf.sh --sample
the-block --commit`, clean and clock-stable, must hold `frame` p99 ≤ 16.6 ms and max ≤ 33 ms,
`sim.block` p99 ≤ 6 ms and `sim.collapse` max ≤ 12 ms. Luca's unpinned run on 2026-10-06
(driver 610.57) read `frame` p99 28.01 / max 57.10, `sim.block` p99 15.49, and the two largest CPU
costs after `frame.submit` were `physics.server.step` p99 7.65 and `physics.client.step` p99 7.62.

The block is two machines in one process: a server and a client, each with its own world, physics,
destruction and replication state. Its tick ran the two halves **in series**, so `sim.block` was
the *sum* of two physics steps, and a real deployment pays only one of those per machine.

Two findings from p2-perf decide this:

1. **`frame.submit` is GPU execution, seen from the CPU.** Split inside `submit_blocking`, on RADV
   (600 frames × 3): fence wait 26.0–27.0 ms p99 of a 26.4–27.3 `frame.submit`, queue submit
   0.08–0.10, and the GPU's own pass total 26.0–26.9. The serialized loop is doing what ADR-0041
   said it does. Pipelining is the lever for that (already built, `--pipelined N`, opt-in, and a
   fingerprint change). It is not taken here.
2. **The two peers' halves share no state between PreSim and PostSim.** The network is read
   before them and written after them. The only shared things are the job pool, the per-step
   bookkeeping (`note_step` → `tick_solve`) and the profiler's zone sink.

## Decision

1. **The server's half runs as one job while the tick's thread runs the client's**, joined before
   PostSim. A job and not a thread, because `JobSystem` may be submitted to from inside a running
   job: the server's physics keeps its parallel solve through the same pool, and there is no
   second pool. Each world's step is deterministic whatever the worker count (ADR-0026), so the
   overlap changes when work happens, not what it computes. The server's `note_step` is captured
   and applied after the join, in the old order: client first, then server.
2. **`ZoneTimelines` parks foreign-thread zones** for the owner to `collect_parked()` after the
   join, bounded at 4096 per collection. Dropping them would have emptied `physics.server.*` from
   the report.
3. **The `sim.block` accounting tree changes shape**, because the old one became false:

   | before | after |
   |---|---|
   | `sim.block = physics.server.step + physics.client.step + rest` | `sim.block = sim.client + sim.join + rest` |
   | | `sim.client = physics.client.step + rest` |
   | | `sim.server = physics.server.step + rest` |

   With the halves overlapping, the old children sum to more than the wall clock. The residual
   would go negative, and `sim.block.unaccounted ≤ 6.0` would pass whatever happened. The old
   single 6.0 limit on "sim work that is not a physics step" is **divided**, 2.0 to each of the
   three new residuals, not granted to each. `sim.join` is the client thread's wait for whatever
   part of the server's half did not fit under its own.

**`sim.block` still means the simulation's wall clock in the frame.** What changed is that the
wall clock now holds `max(client half, server half)` rather than their sum. **The budgets are not
changed**: `frame` 16.6 / 33, `sim.block` 6.0, `sim.collapse` 12.0. No quality setting and no scene
content changed.

## Measured (UNPINNED — RADV iGPU on the same Ryzen 9 9950X3D; the RTX 3060 was mid-driver-upgrade)

Interleaved, 600 frames, 3 runs per arm, 1-minute load 1.1–3.8, `main` (453866b) as control:

| | main | this | spread per arm |
|---|---|---|---|
| `sim.block` p99 | 15.51–15.59 | **8.40–8.45** | ≤ 0.1 |
| `sim.block` max | 15.72–16.04 | 8.81–10.07 | |
| `sim.collapse` max | 15.23 (r2) | **8.37 (r2)** | |
| `frame` p99 (iGPU-bound) | 43.31–44.48 | 36.70–36.84 | ≤ 1.2 |
| `sim.client` p99 | 7.68 (r2) | 7.98 (r2) | contention: +0.3 |
| residuals p99 | 0.46 | 0.30 / 0.12 / 0.20 | limits 2.0 each |

`sim.block` and `sim.collapse` are CPU timelines on the CPU Luca measures on, so they should carry
over. `frame` here is iGPU-bound and does **not**. TSan (`the_block --headless`, validation
layer disabled): 0 reports, all 29 claims hold. With the validation layer enabled, 19 reports
fire, all inside `libVkLayer_khronos_validation` during renderer construction, none in this
change's code.

## Consequences

- **Projected, not measured:** on the 3060, `frame` p99 ≈ 28.0 − (15.5 − 8.4) ≈ **21 ms**. That is
  still over 16.6. `sim.block` p99 ≈ 8.4 is still over 6.0. `sim.collapse` max ≈ 8.4 would pass
  12.0. Gate 7 is **not** closed by this ADR.
- What remains, in size order: the serialized GPU wait (`frame.submit`, about 11 ms p99 on the
  3060), which pipelining takes off the critical path but which is a fingerprint and baseline
  decision for Luca. Then one physics step per half (~7.6 ms each, with contacts ~3.9 and solve
  ~3.2 the largest stages).
- A comparison against the 2026-09-22 baseline is still valid by fingerprint (same preset), but
  `sim.block` before and after measure different topologies. Read `sim.client`/`sim.server` for
  like-for-like per-peer cost.

## What must be checked (Luca's pinned run, after the reboot)

1. `scripts/perf.sh --sample the-block` on the RTX 3060, clock-stable: `sim.block` p99 near 8.4
   and `sim.collapse` max ≤ 12.
2. `foreign_zones` is 0 (no "dropped" line), the three residuals are each ≤ 2.0, and
   `physics.server.step.per_frame` has 600 samples.
3. Whether the `frame` max regression (57.10 vs 31.30) reproduces on a stable clock. The RADV
   bisect cannot see it (no spike on any arm), so it is still unattributed.
