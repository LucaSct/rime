# `docs/perf/` — the measurements, committed

This directory is the engine's performance history: one small JSON per recorded run, appended
never rewritten, each one saying which machine produced it.

It exists because of the split ADR-0035 §2 makes. *How much work does the frame do* is machine-
independent — draw calls, awake bodies, shadow slots re-rendered — so CI gates it on lavapipe,
deterministically, forever (that is the [work ledger](../../engine/core/include/rime/core/diagnostics/work_ledger.hpp),
checked by every sample's `--headless` mode). *How fast does the work run* is not: it is a property
of one GPU, one driver and one build. So it lives here instead, in files a human can diff and cite.

> *"Measure before optimizing; note the measurement in the commit/PR"* has been in CLAUDE.md since
> Milestone 0. This directory is what turns that habit into a ledger.

---

## Taking a measurement

```bash
scripts/build.sh --preset release --cpp-only --no-tests   # Debug numbers mean nothing
scripts/perf.sh --commit                                   # both samples, 1920x1080, into docs/perf/
scripts/perf.sh --sample lit-rooms --frames 1200           # one sample, into a scratch dir
```

`scripts/perf.sh` stamps `RIME_PERF_COMMIT` from git, defaults to the Release build, and names each
report `<date>-<sample>-<gpu-slug>-<sha>.json`. The slug keeps two machines' histories from
colliding; the SHA keeps two runs on one day from colliding, so a before/after pair measured in one
sitting can both be filed. Reports committed under the older SHA-less name remain valid baselines.
A dirty tree is recorded as `<sha>-dirty` rather than attributed to a commit it does not match.

### Pin the GPU clocks first — `perf.sh` will refuse otherwise

`scripts/perf.sh` exits 3 on a GPU whose clocks the driver is still free to move, and prints the
commands for the card it found. On the reference machine:

```bash
sudo nvidia-smi -pm 1            # persistence mode is OFF by default on this box
sudo nvidia-smi -lgc 1837,1837   # the clock EVERY committed baseline encodes -- see below
sudo nvidia-smi -lmc 7501        # both domains; the memory one is the one that bites
# afterwards
sudo nvidia-smi -rgc && sudo nvidia-smi -rmc
```

**1837, not 1785.** This block said `1785` until 2026-10-11 ("85 % of max: a clock the card holds").
Both are supported clocks on the RTX 3060 (max 2100), but every committed baseline carries
`graphics_median 1837.0`, so pinning at 1785 measures a **2.8 % slower** graphics clock than the
number being compared against — inside the 10 % gate tolerance, and therefore a bias that passes
silently rather than failing. Pin at the baseline's own clock.

Both domains, and the memory one is the one that bites. `99-the-block` is CPU-bound — the
simulation is most of the frame — so the GPU is idle most of every frame and the power governor
concludes it has nothing to do. Measured mid-run: core 1837 → 210 MHz and memory 7501 → 405 MHz.
Pinning only the core is not enough: with `-lgc` held rock-steady, memory still sat at 810 MHz of
7501 for an entire run, never boosting, at ~11% of peak bandwidth — and SSR, DDGI and the 1080p
resolves are bandwidth-bound.

The damage is inconsistency rather than slowness, which is why it survives a sanity check. Two
identical 600-frame runs minutes apart agreed on frame p99 (41.48 / 41.58 ms) and disagreed by 2x on
the same pass: `ssr-resolve` p50 1.842 vs 0.856 ms. Pinned, the same pair agrees to 0.1% (0.880 vs
0.879 ms) and every pass above 0.05 ms lands within 1.5%. A baseline taken unpinned encodes the
governor's mood, and every future comparison against it inherits that.

**What is checked is now stability, not pinning** ([ADR-0050](../adr/0050-clock-stability-not-clock-pinning.md)).
Pinning was only ever a proxy for "the clock did not wander during the measurement", it cannot be done
at all on the Pascal card that hosts, and the old pre-run check read an *idle* GPU — which parks in P8
regardless of how it is configured — so it answered a question nobody asked. `perf.sh` now samples both
clocks at 20 Hz **while the benchmark runs**, keeps the samples taken at ≥10% utilisation, and writes
min/median/max/spread plus the sample counts into the report as `gpu_clocks`. A graphics spread above
**2% of its median** refuses the run and blocks filing; the memory domain is recorded but does not gate.

A run with fewer than 10 loaded samples carries `"stable": null` — not a pass. There is no override
environment variable any more (`RIME_PERF_ALLOW_UNPINNED_CLOCKS` is gone): a machine that holds its
clock passes without being pinned, and one that does not is not fit to measure on.

The samples can also be driven directly:

```bash
build/release/bin/lit_rooms --perf --width 1920 --height 1080 \
    --out r.json --baseline docs/perf/2026-08-20-11-lit-rooms-nvidia-geforce-rtx-3060.json
```

**None of this runs in CI, deliberately.** CI renders on lavapipe, a CPU rasterizer where a
millisecond describes the CI machine's mood rather than the engine, and a self-hosted runner on one
desk machine would make every merge hostage to that box's uptime. The trade is named in ADR-0035
§2b: the hardware gate is *procedural*, and the mitigation is that reports are committed, so an
absent measurement is visible in review rather than merely absent.

---

## The reference machine

The numbers in this directory were taken on Luca's workstation unless a report's fingerprint says
otherwise:

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 3060 |
| Driver | NVIDIA 610.43.03 (Vulkan 1.4.341) |
| CPU | AMD Ryzen 9 9950X3D |
| OS | CachyOS (Linux 7.1.5) |
| Build | `release` preset — which is **`RelWithDebInfo`**, not `Release`: optimized with debug info (see CMakePresets.json). No sanitizer; Vulkan validation layers off, since they follow `NDEBUG`. |
| Resolution | 1920×1080 |

A report is only ever compared against another report whose **fingerprint matches** — GPU, driver,
OS, build config, sanitizer, preset and resolution, all of them. Commit and date deliberately do not
participate: they are what differs between the two runs being compared.

That list is not fussiness. Two entries earn their place by having caused real confusion elsewhere:
the **sanitizer**, because an ASan binary is several times slower and would read as a catastrophic
regression against a clean baseline (and #125 showed a sticky CMake cache can make the flag you
configured with and the flag the binary carries disagree — so both are baked in at compile time and
describe the *binary*); and the **driver**, because a Vulkan API version does not change across an
NVIDIA driver update, so without the driver string a driver's performance change would have been
attributed to the engine.

---

## The schema (v1)

```jsonc
{
  "schema": 1,
  "run":     { "sample": "11-lit-rooms", "commit": "95a2c0b", "date": "2026-08-20" },
  "machine": { "gpu": "…", "driver": "…", "os": "linux", "build": "RelWithDebInfo",
               "sanitizer": "off", "preset": "all-lighting-gates",
               "width": 1920, "height": 1080 },

  // Named timelines. Every duration in milliseconds, rounded to three decimals — a run-to-run
  // difference in the fourth is thermal noise, and rounding keeps a git diff readable.
  "distributions": {
    "frame":          { "count": 600, "min_ms": …, "p50_ms": …, "p95_ms": …, "p99_ms": …, "max_ms": … },
    "frame.collapse": { … },   // the same frames again, restricted to the destruction window
    "sim.tick":       { … },   // one whole fixed tick, from the engine's own profile zones
    "sim.physics":    { … },   // 10-destructible-wall only: the physics step, timed on its own
    "frame.declare":  { … },   // per-stage CPU: pass declaration
    "frame.execute":  { … },   //                graph compile + record
    "frame.submit":   { … }    //                the blocking submit, i.e. GPU wall time
  },

  "passes":      { "<pass name>": { "p50_ms": …, "max_ms": … } },   // per-pass GPU time
  "worst_frame": { "index": 288, "ms": 8.05, "passes": { … } },     // that one frame's breakdown
  "ledger":      { "…": 0 }                                         // the run's work ledger
}
```

**There is no mean, anywhere.** Destruction is bursty and the fracture tick is exactly the frame
that must stay smooth; ten 40 ms frames in a 600-frame run move the mean by half a millisecond and
vanish. p99 and max cannot hide them, so the mean is not merely discouraged here — it is
unrepresentable.

**Percentiles are nearest-rank**: p99 of 600 frames is the 594th-slowest frame, a duration that
genuinely occurred, rather than an interpolation between two frames that nothing measured. One
consequence follows and is worth expecting: with fewer than 100 samples p99 *is* max, so a short run
says nothing about a tail. The gate enforces a sample floor rather than letting a lucky 40-frame run
look smooth.

**Every report carries its own work ledger**, and the gate reads it. A fast run on a scene that did
no work is not a pass, it is a broken measurement — so "the frame was quick" always arrives with
"…and here is the work it did". That is m11.7's vacuity lesson applied to a stopwatch.

---

## What the gate checks, and how it fails

Each sample's `--perf` mode carries its own budget, checked at the end of the run; a breach exits
non-zero. Four distinct ways to fail, all of them reported by name:

| Outcome | Means |
|---|---|
| `Breach` | over an absolute ceiling — the product bar (60 Hz at 1080p) |
| `Regressed` | inside the ceiling, but >10% worse than the committed baseline for this machine |
| `Missing` | a rule names a timeline nobody recorded — **a failure, not a skip** |
| `TooFewSamples` | the timeline is too short for the statistic to be evidence |

`Missing` is a failure for the same reason the work ledger's `NOT RECORDED` is: rename a timeline,
forget to rename its rule, and a skipping gate keeps reporting success over a number it can no
longer see. A gate that cannot fail is indistinguishable in a log from one that passes.

And when the baseline comparison does **not** happen — no committed report yet, or one from a
different machine — that is printed as a named status (`not-provided`, `fingerprint-mismatch`),
never inferred from silence. An unremarked skip is how a comparison rots.

---

## Reading the history

```bash
git log --oneline -- docs/perf/                       # when the numbers moved
git diff HEAD~5 -- docs/perf/                         # and by how much
```

Because durations are rounded and keys are stable, a diff between two reports on the same machine is
a readable summary of what got slower and which pass did it.

**Perf-touching bricks commit a `docs/perf/` run.** That line is also in
[CLAUDE.md](../../CLAUDE.md)'s brick-delivery list, so its absence is something a review can notice.

## Every committed `99-the-block` baseline was measured with the GPU clocks PINNED

**So an unpinned run cannot be compared against them, and the comparison will not fail — it will
report a large, convincing regression that is not there.** Verified 2026-10-10.

`2026-10-06-99-the-block-nvidia-geforce-rtx-3060.json` carries

```json
"gpu_clocks": { "graphics_median": 1837.0, "graphics_spread_pct": 0.0,
                "memory_median":   7501.0, "memory_spread_pct":   0.0, "stable": true }
```

A **0.0 %** spread at exactly 1837/7501 MHz is a locked clock, not a lucky one. `scripts/perf.sh`
does **not** pin (it only documents the caveats), so that state came from the box being pinned by
hand at the time.

**What an unpinned run on the same tree looks like**, measured on `f4bff14` with the box otherwise
idle:

| | pinned baseline (2026-10-06) | unpinned (2026-10-10) | |
|---|---|---|---|
| `frame` p99 | 13.705 ms | **22.336 ms** | reported as REGRESSED |
| `frame` max | 15.971 ms | **30.882 ms** | reported as REGRESSED |
| `sim.block` p99 | 8.328 ms | **8.498 ms** | **+2.0 %** |
| graphics clock | 1837 MHz, 0.0 % spread | **1215 MHz median, 98.8 % spread** | gate limit is 2.0 % |

**`sim.block` is the control, and it is what makes the rest interpretable.** It is pure CPU physics
and never touches the GPU, so a clock change cannot move it — and it moved **2.0 %**, inside the
~2 % a perf number is worth across sittings. The CPU side did not regress. The `frame` difference is
the clock state and nothing else.

**Why this card will not hold a clock on this sample.** `99-the-block` runs both client and server
physics (739 bodies, two worlds) and is **CPU-bound**: measured under 108 s of sustained load, GPU
utilisation stayed at 4–44 % and power at **13–17 W of a ~170 W part** at 36 °C, and the graphics
clock sat at **210 MHz** for the first ~70 s before creeping only to ~350 MHz. The governor is not
throttling — it has nothing to do. The card also returns to 210 MHz the instant load stops, so
running `perf.sh` twice in a row does not warm it: every invocation starts parked, its 90-frame
warmup is under a second, and the ramp runs straight through the 600 measured frames.

**Do not fix this by pinning.** That requirement was withdrawn on 2026-09-16 — the box is genuinely
shared, and other computation runs here. The standing replacement is to **filter, not pin**: record a
clock trace beside each run, keep only the frames whose nearest sample has both domains at boost
(`clocks.gr >= 1700`, `clocks.mem >= 7000`), count the frames dropped, and **add a render-only hold
loop after the measured frames, which is what makes a CPU-bound sample boost at all.** A harness that
does this, with 14 runs behind it, is at `~/rime-perf-harness-m17.8b/`. Known hazard, unsettled:
filtering on boost may select different frame populations per arm, since boost correlates with the
treatment — check retained/dropped counts per arm before trusting a delta.

**Consequence for M18 gate 7 — RESOLVED by the owner, 2026-10-11.** The gate's row says
"clock-pinned tree" while the 2026-09-16 ruling forbade pinning, so as written the gate could not be
satisfied by any procedure the project was allowed to use. Luca's decision: *"run it on this machine
when its quiet and i pin it with the commands."*

So **gate-7 runs are pinned by exception, by hand, with the owner present, on an otherwise quiet
box** — using the three commands at the top of this file at exactly 1837/7501. What that buys is the
thing the alternatives could not: the measurement stays **directly comparable to every baseline
already committed**, so M18's history is not invalidated and no tolerance has to be re-derived.

The 2026-09-16 withdrawal is **narrowed, not reversed**, and the distinction is the whole point:

- **A gate-7 or terrain budget run** — pinned, by hand, owner present, box quiet. Rare, attended,
  and comparable to history.
- **Everything else, including any unattended or routine run** — must still work without pinning,
  because the box is shared and Luca is not always at it. The **filter, not pin** procedure above
  remains the standing answer there, with its unsettled population hazard intact.

What has *not* changed: `scripts/perf.sh` still does not pin and must not start, because a script
that silently acquires root to change a shared machine's clocks is the wrong shape. It refuses an
unstable clock (graphics spread > 2 %), and that refusal is what makes the pin a precondition the
operator satisfies rather than a side effect the tool causes. An unpinned `99-the-block` run remains
a **probe**, with `sim.block` the only part of it comparable to history.

## The gate-7 run, pinned, 2026-10-11 — the `frame` clause is MET and the simulation gate is not

First pinned run since the baselines, taken under the owner's ruling above: clocks pinned by hand at
1837/7501, box quiet, clean `main` tree at `bdd3be0`, Release, 1920x1080, 600 frames. It **filed**,
which is the part that matters procedurally — `perf.sh`'s contention watcher found no competitor and
the graphics spread was 0.0 %, so the run was not discarded.

**The clock fingerprint is byte-identical to the baseline's** — graphics 1837/1837/1837 at 0.0 %
spread, memory 7501 at 0.0 %, 134 of 140 samples loaded. That is exactly what pinning bought: a run
directly comparable to committed history, which an unpinned run is not.

| metric | budget | 2026-10-06 (pinned) | 2026-10-11 (pinned) | |
|---|---|---|---|---|
| `frame` p99 | **16.600** | 13.705 | **14.619** | **inside budget**, +6.7 % vs baseline |
| `frame` max | 16.600 | 15.971 | **15.517** | inside budget, and **-2.8 %** |
| `frame.player` p99 | — | 13.329 | 14.068 | +5.5 % |
| `sim.block` p99 | **6.000** | 8.328 | **8.533** | **BREACH**, +42 % over budget |
| `sim.collapse` p99/max | — | 8.154 | 9.980 | flagged REGRESSED (+22.4 %) |

**What this closes and what it does not.** ADR-0043 gate 7 requires a clean-tree clock-pinned report
to meet "the unchanged ratified `frame` p99/max **and** simulation gates". The `frame` half is met —
for the first time since the clause was written into M13, carried unpaid through M17 (28.376 ms on
2026-09-22) and inherited by M18. The simulation half is not: `sim.block` p99 is 8.533 ms against a
6.000 ms budget. So **gate 7 stays open, and the remaining miss is isolated to the simulation gate**,
which is a much smaller and better-understood target than "the block is 1.7x over frame budget".

`PerfGate::format` prints only violations (`perf_report.cpp:1415`), so the absence of a `frame` line
is the evidence it passed — not an omission.

### Do not trust the +22.4 % `sim.collapse` regression, and the reason generalises

Two independent facts say it is not code:

1. **`engine/physics` and `engine/destruction` are byte-identical** between `3adeee9` and `bdd3be0`
   (`git diff --stat 3adeee9 bdd3be0 -- engine/physics engine/destruction` is empty). Nothing that
   computes `sim.block` or `sim.collapse` changed. The `engine/core` changes in that range are
   additive (world bounds, entity refs, the perf report's own fields).
2. **`sim.collapse` p99 == max in BOTH runs** (8.154/8.154 and 9.980/9.980). The collapse window is
   **90 frames**, so p99 *is* the top sample — this is one worst frame against one worst frame, not a
   distribution that moved. Meanwhile `sim.collapse` p50 moved +3.3 %.

The pattern across the whole run is the tell: **every** simulation median moved +3 to +6 % and
**every** simulation tail moved +13 to +22 %, while the `frame` tail *improved* by 2.8 %. A uniform
shift with no code change is an instrument, not a regression.

### The instrument: we pinned the GPU for a sample that is CPU-bound

This is the actionable finding, and it is the same error this file already records twice in other
forms — controlling the wrong variable and believing the number anyway.

`99-the-block` is **CPU-bound** (this file says so above: 4–44 % GPU utilisation, 13–17 W of a ~170 W
part). The pin fixes the GPU. **Nothing fixes or even records the CPU**, and on this box the CPU is a
hostile instrument:

- **AMD Ryzen 9 9950X3D, 16C/32T, and it is HETEROGENEOUS**: two L3 instances of 128 MiB total
  (`cpu0` shares L3 with `0-7,16-23`; `cpu8` with `8-15,24-31`). One CCD carries the 3D V-Cache. For
  a cache-resident workload like island solving, **which CCD the simulation threads land on changes
  the answer**, and nothing pins them.
- Governor `performance`, boost **on**, and per-core clocks measured live spanning **2024 to
  5721 MHz** — a 2.8x spread across cores in one sample.
- The committed report's `machine` block is `{gpu, driver, os, build, sanitizer, preset, width,
  height}`. **There is no CPU field anywhere in the JSON.** So the one device that determines the
  metric still missing its budget is neither controlled nor recorded, and a future reader cannot tell
  two runs apart on it.

**So the next step for gate 7's simulation half is CPU control, not more GPU pinning.** Concretely, and
in the order worth doing: record CPU model, governor and per-core clock min/median/max/spread in the
report fingerprint, exactly as `gpu_clocks` already does — an unrecorded precondition is the thing
this whole file is about; then pin the simulation threads to one CCD (`taskset -c 0-7,16-23` or the
V-Cache die) so cache locality stops varying between runs; then re-baseline `sim.block` under that
procedure before judging any delta against it. Until then, a `sim.block` *delta* is not evidence,
though the **42 % breach is far larger than the ~6 % variance observed here and is therefore real**.

### CORRECTION, same day: the `frame` clause was NOT met "for the first time" on 2026-10-11

The section above claimed the 2026-10-11 run met the `frame` clause *for the first time since M13
wrote it*. **That is wrong, and it was wrong in the commit message, the PR and the ROADMAP row too.**
Caught by an external consult that asked why `sim.block` would drift 8.328 -> 8.533 in five days while
the frame clause "first" passed — the right question, and the answer is that it had already passed.

Every committed `99-the-block` report, `frame` p99 against the 16.600 ms budget and `sim.block` p99
against 6.000:

| report | `frame` p99 | | `sim.block` p99 | | GPU pinned |
|---|---|---|---|---|---|
| 2026-08-30 | 35.598 | fail | 25.491 | fail | unrecorded |
| 2026-09-05 | 28.915 | fail | 24.555 | fail | unrecorded |
| 2026-09-06 | 20.976 | fail | 16.881 | fail | unrecorded |
| 2026-09-07 | 19.624 | fail | 15.435 | fail | unrecorded |
| 2026-09-18 | 32.605 | fail | 15.634 | fail | unrecorded |
| 2026-09-22 | 28.376 | fail | 15.692 | fail | unrecorded |
| **2026-10-06** | **13.705** | **PASS** | 8.328 | fail | **yes** |
| **2026-10-11** | **14.619** | **PASS** | 8.533 | fail | **yes** |

So the `frame` clause was first met on **2026-10-06**. 2026-10-11 is the **second** such run, and its
number is **worse** (+6.7 %). What today actually established is narrower and worth stating as such:
the clause *still* holds on a tree eleven PRs newer, under a now-ratified procedure, and gate 7's
remaining miss is isolated to simulation.

**And the stronger point, which the wrong claim obscured.** The only two runs that pass are the only
two runs that were **pinned**. Every earlier report has no clock record at all. We also know from this
very file what unpinned does to this metric: the 2026-10-10 unpinned probe on a near-identical tree
read `frame` p99 **22.336** where the pinned run reads 14.619 — a **1.53x inflation** from the
instrument alone. The 2026-09-22 -> 2026-10-06 improvement is 28.376 -> 13.705, a factor of **2.07**.

**Those two factors are the same order, so this data cannot separate "the engine got faster" from
"the measurement got honest."** Real work did land in that window (m17.5's one-step-per-tick change
alone was worth -6.4 ms, measured), so the truth is certainly some of both — but the split is not
recoverable from these reports, and no claim resting on that split should be made. `sim.block`, which
is clock-independent, fell 15.692 -> 8.328 across the same boundary, which is the best available
evidence that genuine work landed.

`sim.block` has **never** passed, in eight reports: 25.491 -> 8.533.

### What two independent consults said about the reasoning above (2026-10-11)

`gpt-6-astra` (via codex, read-only repo access) and `claude-fable-5-1` (via the gateway, excerpts
inlined) were each asked for their **own** proposal, without being shown mine or each other's. Two
things they agreed on, reached by different routes, which makes the agreement worth more than a
shared opinion:

1. **The 1.09x island-parallelism ceiling is not established by the evidence behind it.** This file's
   sibling memory and ADR reasoning derive it from "~92 % of awake bodies are in ONE island". Both
   said that does not follow: *"body share is not solver-time share"* (astra), *"without the scaling
   exponent, 1.09x is a width argument, not a cost argument — that's the missing number"* (Fable). The
   conclusion may still be right; the stated basis does not support it. **The missing measurement is
   per-island solver time against body count**, not body count alone.
2. **Two named results, not one.** A pinned number for regression detection and an unrestricted one
   for shipping, with affinity never silently satisfying the shipping gate. Fable would rename the
   gated metric `sim.block@pinned` to make the redefinition explicit; astra would keep both timelines
   joint rather than reporting them independently, because *"independently reported p99 values cannot
   establish which work causes missed frames"* — the `frame` p99 tick and the `sim.block` p99 tick are
   not the same tick, so one cannot attribute the other.

Both also warned the tail statistics are thin: **600 samples leave ~6 observations in the top 1 %**,
so a small p99 difference between single runs is not evidence. Repeat whole scripted runs and report
variance before trusting any `sim.block` delta.

Where they differed, and the useful part:

- **Which CCD.** Astra refused to guess and would A/B cache-CCD vs frequency-CCD vs worlds-split with
  randomised run blocks and confidence intervals over whole runs. Fable named a prior — *"602 bodies
  plus contacts is a few MB, fits 32 MiB either way, so the high-clock CCD probably wins, but that is
  exactly the claim the test exists to kill"* — and proposed `perf stat` with an explicit decision
  rule (cache wins only if IPC rises >5 % and LLC-miss rate falls materially). **Take Fable's decision
  rule inside astra's experiment design**: a stated threshold is what stops the A/B being read as
  taste, and randomised blocks are what stop thermal drift being read as an effect.
- **MEASURED by astra, and verified here**, which no amount of reasoning would have produced:
  `0-7,16-23` share **96 MiB** L3 and `8-15,24-31` share **32 MiB**, so the first group is the
  3D V-Cache CCD. Note this also kills Fable's identification method on this box: it suggested finding
  the V-Cache die by its lower clock ceiling, but `cpuinfo_max_freq` reads an identical 5756452 kHz on
  **both** CCDs here. **L3 size identifies them; frequency ceiling does not.**
- **The budget.** Fable: 6.000 is *"not binding on the user-visible budget — it is a headroom
  allocation, not a need"*, and until someone states what the reclaimed 2.5 ms buys, the gate is
  taste. Astra: keep 6.000 **provisionally** and measure the tail tick's critical path first, then
  bound the feasible improvement from measured removable time. **Astra's order is the right one** —
  "the gate is taste" may well be true, but it is a conclusion to earn from a critical-path
  measurement, not a reason to skip taking one.

Three unique points worth acting on:

- **Fable: a determinism contract is not a quality contract.** A Jacobi or hybrid velocity solver
  changes convergence and stacking stiffness, so within-island parallelism needs a **correctness** gate
  (does the stack still stand, does it settle), not only the bitwise replay contract ADR-0026 already
  specifies. The engine's existing determinism apparatus would pass a solver that visibly ruins
  stacking.
- **Fable: is the tail tick a steady state or a merge/wake spike?** If the 602-body island is formed by
  a merge event, *"a sleep/split policy beats a parallel solver by an order of magnitude in effort."*
  Cheap to check and it changes which brick is next.
- **Astra: fingerprint more than the clock** — residency, migrations, context switches, temperature,
  throttling, power limits, kernel, compiler options, executable hash, workload hashes. Today's CPU
  fingerprint records model, governor, boost, L3 topology and clock spread; migrations and residency
  are the next most valuable and are not there yet.
