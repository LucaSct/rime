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
report `<date>-<sample>-<gpu-slug>.json` so two machines' histories coexist here without colliding.
A dirty tree is recorded as `<sha>-dirty` rather than attributed to a commit it does not match.

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
| Build | `release` preset, no sanitizer, validation layers off (`NDEBUG`) |
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
  "machine": { "gpu": "…", "driver": "…", "os": "linux", "build": "Release",
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
