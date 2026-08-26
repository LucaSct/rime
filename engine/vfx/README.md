# rime::vfx — the destruction dust stub (M8.4)

A deliberately small, **deletable** CPU particle field that turns destruction events into visible
feedback: when a part breaks or an island detaches, a puff of billboard dust blooms at the break and
drifts away. It was a *stub* in the honest sense — and since **m13.1a** it is a stub that is
actually drawn: `render::FxParticlePass` renders these particles as additive HDR billboards. What
remains stubby is the *content* (one family, no atlas, no GPU sim), not the pipeline.

## The idea

- **`DustField`** — a capped pool of `DustParticle`s. `emit_burst(min, max, intensity)` blooms a puff
  filling a world-space box (a broken part or island's AABB); `simulate(dt)` drifts, ages, and retires
  them. Deterministic — a fixed SplitMix64 stream drives the scatter — so two fields fed the same
  events hold identical particles.
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

It is **not wired to destruction**: the fan-out glue (a `DestructionEvent` → `emit_burst`) lives in
the consumer, so the dependency arrow never points from vfx into destruction. Removable feature module
(guardrail 2): depends on `rime::core` only, and nothing depends on it.

## Building & testing

Built as part of the engine (`scripts/build.sh`). The test is pure-CPU and runs on every CI OS plus
ASan/UBSan and TSan:

```bash
ctest --preset dev -R rime_vfx_tests
```
