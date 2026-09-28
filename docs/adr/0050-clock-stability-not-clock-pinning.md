# ADR-0050 — Clock stability, not clock pinning

- **Status:** accepted
- **Date:** 2026-09-28
- **Decided by:** Luca
- **Amends:** [ADR-0047](0047-two-machines-and-the-starbase-tier.md) §2, whose precondition is replaced
- **Relates to:** [ADR-0035](0035-vision-demo-m12.md) §2c (the perf fingerprint), [ADR-0041](0041-the-visual-bar-m17.md)
  Ruling 4 (a quiet wrong comparison is worse than no comparison)

## Context

ADR-0047 §2 ruled that the starbase tier's ratified budget comes from **the first clock-pinned
`perf.sh` run** in CT 122. That precondition turns out to be unmeetable on the hardware it was written
for, and the check that was supposed to enforce it was itself measuring the wrong thing.

**Measured on the host, 2026-09-28, GTX 1060 6GB, driver 580.178.04:**

- `nvidia-smi -lgc`, `-lmc` and `-ac` all print
  `Setting ... is not supported for GPU 00000000:01:00.0`. Pascal cannot be clock-pinned by any of the
  three mechanisms. Only `-pl` (power limit, 60–140 W, default 120 W) is settable.
- **Each of those commands then prints `Treating as warning and moving on` and exits 0.** An
  exit-status check on them proves nothing. This is the same trap as grepping a build log for `error:`
  — the tool reports failure in its output and success in its status — and it was walked into inside a
  script written to avoid exactly that class of mistake.
- `scripts/perf.sh`'s own `check_gpu_clocks()` compared `clocks.current.*` against `clocks.max.*`
  **before the run, at idle**. An idle NVIDIA GPU sits in power state P8 regardless of configuration:
  this card reads **139 MHz of 1911 MHz** when nothing is running. So the check reported "not pinned"
  about a machine that would spend the entire measured workload near its maximum clock.
- Under real load (`lit_rooms --headless`, 81% utilisation): **P0, 1898–1911 MHz, a 13 MHz spread =
  0.7% of the median**, 89 W of a 120 W limit, 55 °C, over 60 samples in 30 s. 13 MHz is **one bin** in
  the card's own supported-clock table.

So the card is not merely unpinnable — at 55 °C and 89 W it has so much thermal and power headroom that
it boosts to maximum and holds it. It is already as stable as a pin would make it.

## Decision

**The precondition becomes clock-*stability*-verified rather than clock-*pinned*.**

`perf.sh` samples the graphics and memory clocks **during the measured run** — not before it — and
records min, median, max and spread-as-a-percentage-of-median for both domains in the fingerprinted
report. A run whose **graphics** spread exceeds **2% of its median** is not trustworthy and the report
is refused, exactly as an unpinned run was refused before.

Three implementation properties are part of the decision, because the gate is wrong without them:

- **Only samples taken under load count.** A benchmark process is not busy for its whole lifetime:
  `lit_rooms --perf` measures 600 frames in ~1.1 s (measured, 3060) but first creates a device,
  compiles pipelines and uploads assets with the GPU at 0–1% utilisation. On a card that cannot be
  pinned that window reads the *idle* clock — 139 MHz against a 1911 MHz median — so a naive
  whole-lifetime sampler would report a ~93% spread and fail every run on exactly the machine this
  ADR exists to unblock. A sample counts when utilisation is **≥10%**.
- **Too short a loaded window yields `"stable": null`, which is not a pass.** Fewer than **10**
  loaded samples is not a distribution; the report then carries the numbers and a null verdict, and
  the run is not a verified-stable one. Silence and success must not be the same answer.
- **The sampler is one process at 20 Hz** (`nvidia-smi -lms 50`), not one process per sample. This
  script also refuses to measure on a box that is not quiet, so a guard that forked twice a second
  would contend with the run it is judging; measured, the sampler costs 0.0% CPU.

- The **memory** domain is measured and reported but does not gate. ADR-0047's predecessor discipline
  was right that both domains matter — a memory clock that moves while the graphics clock does not is a
  real signal — but gating on it would refuse runs for a reason we have not yet shown to distort a
  frame-time distribution.
- The bound is 2% against 0.7% measured: roughly three times the observed variation, which is slack
  enough not to flap and tight enough that a genuinely wandering machine fails.
- The script **sets no clocks**. It observes. Given the exit-0 trap above, any code path that tried to
  pin and then believed the result would be worse than no check at all.
- A machine that *can* pin passes this gate trivially, so the new check strictly subsumes the old one.

## Why this is a better gate, not a concession

Pinning was never the property we wanted; it was a proxy for "the clock did not wander during the
measurement". Measuring the property directly is better in three ways:

1. **It works on hardware that cannot pin**, which is the machine that actually hosts (ADR-0047 §3).
2. **It catches failures a pin check cannot.** A pinned GPU that thermally throttles mid-run passes a
   pin check and produces a contaminated distribution; a stability check sees the throttle.
3. **It reports a number rather than a boolean.** ADR-0041 Ruling 4 exists because a comparison whose
   preconditions differ silently is worse than no comparison; a recorded spread makes the precondition
   visible in the report instead of implied by its existence.

## Consequences

- **The starbase bar is unblocked.** It can be ratified from the first run in CT 122 that passes the
  stability gate, which is a run that can actually be produced. It still must not be taken from
  ADR-0035's 3060 number, which the block already misses on the faster machine.
- Every `docs/perf/` report gains the clock statistics. Older reports do not have them, so a comparison
  across that boundary is between a measured precondition and an assumed one, and should say so.
- **The evidence behind the 2% bound is narrow and the ADR says so rather than implying otherwise:**
  one workload, 30 seconds, one container, a quiet host, 55 °C. A longer run, a hotter room, or a
  heavier workload is not covered by it, and CT 110 shares this GPU (ADR-0047 §3) so a concurrent
  OpenCL job could change the picture. If the gate starts flapping, the right response is to widen the
  evidence before widening the bound.
- The measurement discipline generalises: **read a clock under load or not at all.** An idle reading
  describes the governor's idle behaviour and nothing about the run.

## What was rejected

- **Ratify from unpinned runs and label them.** Honest, but it throws away a check instead of fixing
  one, and leaves every future reader wondering how unstable "unpinned" was on the day.
- **Have no starbase bar.** Gives up the thing ADR-0047 made the hosted tier separate for, on the basis
  of a precondition that turned out to be the wrong precondition.
- **Pin via the power limit instead.** `-pl` is settable and does bound boost behaviour, but a power
  limit is not a clock and a run capped at 60 W would measure a different machine. It stays at its
  120 W default, which is part of the fingerprint rather than a knob this script turns.
