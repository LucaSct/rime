# rime::vfx — the destruction dust stub (M8.4)

A deliberately small, **deletable** CPU particle field that turns destruction events into visible
feedback: when a part breaks or an island detaches, a puff of billboard dust blooms at the break and
drifts away. It is a *stub* in the honest sense — the real GPU-driven FX system (track **fx1**)
replaces this whole module.

## The idea

- **`DustField`** — a capped pool of `DustParticle`s. `emit_burst(min, max, intensity)` blooms a puff
  filling a world-space box (a broken part or island's AABB); `simulate(dt)` drifts, ages, and retires
  them. Deterministic — a fixed SplitMix64 stream drives the scatter — so two fields fed the same
  events hold identical particles.
- **`coverage()`** — a cheap CPU proxy (Σ size²·alpha) for how much screen the dust covers. It jumps
  on a burst and decays to zero as the puff ages out; the M8.6 sample self-checks this witness (its
  peak coverage on the break).

**GPU-free by construction.** The simulation lives here; the actual additive *draw* pass stays deferred
to the real FX system (**fx1**). The M8.6 sample drives this field from the destruction fan-out and
self-checks its `coverage()`, but renders the wall + debris via per-part render leaves, not the dust.
The budget is a hard cap (default ~200 particles) so a demolition storm cannot unbound it —
m8.5's budget discipline, in miniature.

It is **not wired to destruction**: the fan-out glue (a `DestructionEvent` → `emit_burst`) lives in
the consumer, so the dependency arrow never points from vfx into destruction. Removable feature module
(guardrail 2): depends on `rime::core` only, and nothing depends on it.

## Building & testing

Built as part of the engine (`scripts/build.sh`). The test is pure-CPU and runs on every CI OS plus
ASan/UBSan and TSan:

```bash
ctest --preset dev -R rime_vfx_tests
```
