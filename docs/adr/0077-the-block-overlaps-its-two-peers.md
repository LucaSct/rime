# ADR-0077: the block runs its server and client halves at once — and what `sim.block` now means

- Status: **Accepted** — the pinned RTX 3060 run it asked for has been made; see "What was checked"
- Date: 2026-10-06 (provisional), 2026-10-06 (accepted on the pinned run)

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

## Measured, development arm (UNPINNED — RADV iGPU on the same Ryzen 9 9950X3D; the RTX 3060 was mid-driver-upgrade)

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

## Measured, the gate's machine (PINNED — RTX 3060, driver 615.71.09)

Both arms are committed reports, so anyone can re-read them rather than trust this table:
control `docs/perf/2026-10-06-99-the-block-nvidia-geforce-rtx-3060.json` on `main` (453866b),
treatment the same path on this branch (3adeee9). 600 frames each, preset
`block-all-lighting-gates`, 1920×1080, RelWithDebInfo, sanitizer off. Clocks pinned with
`nvidia-smi -pm 1`, then `-lgc 1837,1837`, then `-lmc 7501,7501` as **three separate commands**:
both runs report graphics 1837 MHz median, **0.0 % spread, `stable: true`**.

| | `main` 453866b | this 3adeee9 | budget | |
|---|---|---|---|---|
| `frame` p99 | 20.958 | **13.705** | 16.6 | **now passes** |
| `frame` max | 21.377 | **15.971** | 33 | passes |
| `sim.block` p99 | 15.441 | **8.328** | 6.0 | **still over** |
| `sim.collapse` max (n=90) | 15.426 | **8.154** | 12.0 | **now passes** |
| `frame.submit` p99 | 4.112 | 3.677 | — | |
| `physics.server.step` p99 | 7.668 | 7.891 | — | contention: +0.2 |
| `physics.client.step` p99 | 7.548 | 7.947 | — | contention: +0.4 |
| `sim.join` p99 / max | — | 1.095 / 7.852 | — | new zone |

A second pinned run of the treatment read `sim.block` p99 8.64, so take 8.3–8.6 as the arm's
spread rather than 8.328 as a point.

**Why pinning was not optional.** Unpinned runs on this card read a 735–742 MHz median with a
**~208 % spread** — they measured the clock ramp as much as the engine. That is where the earlier
`frame` p99 of 28.01 and max of 57.10 came from.

**A correction this run forces on the "Consequences" below.** The provisional ADR put the
serialized GPU wait at "about 11 ms p99 on the 3060" and named it the largest remaining cost.
Pinned, `frame.submit` p99 is **4.112 on `main` and 3.677 here** — the 11 ms was the card ramping,
not the queue. So the ordering is reversed: after this change the largest remaining cost in
`sim.block` is **one physics step** (p99 ≈ 7.9, of which contacts 4.101 and solve 3.222 on the
server half), not the submit. Pipelining would not move `sim.block` at all.

## Consequences

- **The projection, and what it got right and wrong.** It read: "on the 3060, `frame` p99 ≈ 28.0 −
  (15.5 − 8.4) ≈ **21 ms**, still over 16.6". The `sim.block` half held — projected 8.4, measured
  8.328–8.64 from the iGPU arm, across a different GPU and a driver upgrade. The `frame` half did
  not, and in our favour: measured **13.705**, under the 16.6 budget. The error was in the
  subtrahend, not the model — it extrapolated from an unpinned 28.01 that was mostly clock ramp.
  **Projections anchored to an unpinned number inherit its spread; re-anchor before trusting one.**
- **Gate 7 is still not closed, but it is now one metric wide.** `frame` and `sim.collapse` both
  pass pinned. `sim.block` p99 8.3–8.6 against a 6.0 budget is what remains.
- What remains, in size order, measured pinned: **one physics step** (`physics.*.step` p99 ≈ 7.9,
  contacts 4.101 and solve 3.222 the largest stages). Since the two halves now overlap, `sim.block`
  cannot fall below one step plus the join, so **no further scheduling change reaches 6.0** — the
  step itself has to get cheaper, or the budget has to be re-argued against a 32-participant
  2-world sample. The serialized GPU wait is *not* next (`frame.submit` p99 3.677); pipelining
  remains available but would move `frame`, which already passes, and not `sim.block`.
- A comparison against the 2026-09-22 baseline is still valid by fingerprint (same preset), but
  `sim.block` before and after measure different topologies. Read `sim.client`/`sim.server` for
  like-for-like per-peer cost.

## What was checked (the pinned run, 2026-10-06)

All three items the provisional ADR asked for, read back out of the committed report:

1. **`sim.block` p99 near 8.4, `sim.collapse` max ≤ 12** — ✅ 8.328 (8.64 on a second run) and
   8.154 against 12.0.
2. **Residuals ≤ 2.0 each, `physics.server.step.per_frame` at 600 samples, no dropped foreign
   zones** — ✅ all six residuals in the report are 0.002–0.314 p99 (`sim.block.unaccounted` 0.314,
   `sim.server.unaccounted` 0.194, `sim.client.unaccounted` 0.123, `frame.render.unaccounted`
   0.022, `frame.unaccounted` 0.004, `frame.submit.per_frame.unaccounted` 0.002), each against its
   own 2.0 limit. `physics.server.step.per_frame` n=600 and `sim.collapse` n=90. On the foreign
   zones: the counter is not a report field — the sample prints a warning line only when it is
   non-zero (`samples/99-the-block/main.cpp:2948`), and the run printed none. What the committed
   artefact proves by itself is that `physics.server.*` arrives complete at n=600, which is what
   the parking in Decision 2 buys, since those zones close on a foreign thread. **If you want this
   checkable from the report alone, the counter needs a field** — worth doing before the next ADR
   leans on it.
3. **Whether the `frame` max regression (57.10 vs 31.30) reproduces on a stable clock** — ✅ **it
   does not.** Pinned `frame` max is 15.971 here and 21.377 on `main`, both far under the 31.30
   that the regression was measured against. The 57.10 was the unpinned card. Nothing remains
   unattributed; the RADV bisect saw no spike because there was no spike to see.
