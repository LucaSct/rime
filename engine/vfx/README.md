# rime::vfx — the destruction dust stub (M8.4)

A deliberately small, **deletable** CPU particle field that turns destruction events into visible
feedback: when a part breaks or an island detaches, a puff of billboard dust blooms at the break and
drifts away. It was a *stub* in the honest sense — and since **m13.1a** it is a stub that is
actually drawn: `render::FxParticlePass` renders these particles as additive HDR billboards. What
remains stubby is the *content* (one family, no atlas, no GPU sim), not the pipeline.

## The idea

- **`ParticleField`** — a capped pool of `Particle`s. `emit_burst(min, max, intensity)` blooms a puff
  filling a world-space box (a broken part or island's AABB); `simulate(dt)` drifts, ages, and retires
  them. Deterministic — a fixed SplitMix64 stream drives the scatter — so two fields fed the same
  events hold identical particles. (`DustField`/`DustParticle` remain as aliases: the old names were
  accurate while dust was the only family, and a class that emits muzzle flashes should not be called
  a dust field.)
- **Three families** (m13.1b) — `impact_dust()`, `lingering_smoke()`, `muzzle_flash()`, each an
  `EmitParams` handed to the four-argument `emit_burst`. They differ in ways a test can name rather
  than in taste: smoke has **negative gravity** (it rises) and **growth** (it expands as it
  dissipates, so a long-lived puff does not just fade in place); a flash lives under a tenth of a
  second and has no gravity at all, because nothing falls in 60 ms. Gravity and growth are stored
  **per particle**, which is what lets one field — and therefore one draw — hold all three at once
  while smoke rises and dust settles in the same puff.
- **`coverage()`** — a cheap CPU proxy (Σ size²·alpha) for how much screen the dust covers. It jumps
  on a burst and decays to zero as the puff ages out; the M8.6 sample self-checks this witness (its
  peak coverage on the break).

**GPU-free by construction, and it stays that way.** The simulation lives here; the additive *draw*
pass lives in [`rime::render`](../render/README.md) as `FxParticlePass` — **landed at m13.1a**, which
is what finally put pixels behind `coverage()`. The arrow still only points one way: vfx does not
know rendering exists, and render does not know vfx exists. Converting a `DustParticle` into a
`GpuParticle` is the CONSUMER's glue, exactly like the destruction fan-out below.

`coverage()` is now a *checked* witness rather than a promised one: the m13.1a proof renders a burst
and shows that on-screen radiance and this scalar rise and decay together (20.27 → 3.64 against
0.078 → 0.011). The budget is a hard cap (default ~200 particles) so a demolition storm cannot
unbound it — m8.5's budget discipline, in miniature — and the draw side carries its own independent
cap for the same reason.

It is **not wired to destruction, and not wired to weapons**: the fan-out glue (a
`DestructionEvent` or a shot → `emit_burst`) lives in the consumer, so the dependency arrow never
points from vfx into either. `tests/destruction/events_test.cpp` carries the m13.1b version — a
`PartDied` becomes dust, an `IslandDetached` becomes dust *and* smoke, a shot becomes a flash at the
muzzle.

One authoring lesson from writing that glue, recorded because it is the kind of thing that ships:
scaling the smoke purely by an `IslandDetached`'s `magnitude` looks obviously right and emits
**nothing**, because an island can detach with a magnitude of exactly 0 — the damage impulse went to
the part that was struck dead, and the event is explicitly not emitted for a killed part's own chunk.
A structural collapse should always smoke; the impulse scales it *up*. Multiply instead of flooring
and the quietest collapses — a wall simply giving way — are the ones with no smoke at all.

Removable feature module (guardrail 2): depends on `rime::core` only, and nothing depends on it.

## Building & testing

Built as part of the engine (`scripts/build.sh`). The test is pure-CPU and runs on every CI OS plus
ASan/UBSan and TSan:

```bash
ctest --preset dev -R rime_vfx_tests
```
