# Destruction — design note (M8)

Companion to `engine/destruction` (M8) and the fracture cook in `tools/asset-pipeline`. Like
[`physics.md`](physics.md), this is the running design record — it grows a section per brick, in the
order the bricks land. The *decisions* live in [ADR-0029](../adr/0029-destruction-model.md) (and the
physics substrate in [ADR-0026](../adr/0026-physics-core.md)/[0027](../adr/0027-convex-hull-shapes.md)/
[0028](../adr/0028-compound-shapes.md)); this note covers the *systems* reasoning — the pipeline, the
data, and the trade-offs — and the math derivations get their own notes as they land.

Current coverage: the model (M8.0, this note's overview + ADR-0029). The cook (M8.1), the runtime
(M8.2), damage/connectivity/fracture (M8.3), the event fan-out (M8.4), lifetime/budgets (M8.5), and
the proof sample (M8.6) each add a section as they ship.

## The model (M8.0)

Destruction in Rime is **not** a special-case physics stage — it is a consumer of the rigid-body core
that M7 built to be its substrate. The whole pipeline is a chain of shipped seams:

```
  cook (Rust, offline)            runtime (C++, engine/destruction)
  ─────────────────────           ─────────────────────────────────────────────────────
  source mesh                     load Destructible asset
    → seeded Voronoi parts          → register_hull per part, register_compound the whole   (once/pattern)
    → convex part hulls             → stand an instance = ONE static compound body
    → bond + anchor graph           → accumulate contact impulse (+ explicit ops) → per-part health
    → per-part render meshes         → part dies → connectivity solve (union-find over live bonds)
    → Destructible (RMA1)            → unsupported parts detach → BODY SWAP → debris (hull/compound bodies)
                                     → one event stream (PartDamaged/PartDied/IslandDetached/DebrisSettled)
                                       fans out to VFX + audio + gameplay
```

The load-bearing ideas, each pinned in ADR-0029:

- **Intact = one static compound body.** A fracture *pattern* (its parts + graph) is cooked once and
  registered once (ADR-0028's compound *is* the intact destructible, ADR-0027's hull store *is* the
  debris shape economy). Instances share the registered geometry by id.
- **Damage is contact impulse.** `ContactEvent.normal_impulse` is the solver's summed ∫F·dt, and
  `child_a`/`child_b` already name the struck part (M7.9/M7.12). Above a cooked threshold (which
  fences the `m·g·dt` resting-support case) it is damage to that part. A CCD projectile's arrest
  (M7.10) is the same signal; an explosion is `overlap_sphere` + a radius→part damage op + an
  `apply_impulse` push (M7.7).
- **Fracture is a body swap.** Compounds are immutable, so a broken wall is *replaced*: destroy the
  instance body, re-register the anchored remainder, and spawn each detached island as a new dynamic
  body (a hull for one part, a runtime compound for several). This is why the hull/compound stores
  must learn to release ids (M8.5).
- **Determinism, end to end.** The damage → fracture path is a deterministic function of its inputs
  (canonical op order, ascending-part-id remainders, `world_hash`-stable free-list ids) — the exact
  contract M11 networked destruction replays events against.
- **One event stream, many listeners.** Health transitions publish through a generic double-buffered
  `EventChannel<T>` in canonical order (the M7.9 "data, not callbacks" discipline, one layer up); VFX,
  audio, and gameplay subscribe independently. M8 ships the seam plus a dust stub and a null-audio
  backend — the real systems are Track FX and `engine/audio` later.

See ADR-0029 for the eight decisions and their rejected alternatives; the brick sections below fill in
the how as each lands.

## The fracture cook (M8.1)

*(pending — seeded-Voronoi partition, quickhull backstop, the `Destructible` asset, bond/anchor
graph; math in `docs/math/voronoi-fracture.md`.)*

## The runtime: load, stand, bind (M8.2)

*(pending — `engine/destruction` module, pattern registration, the compound instance, per-part render
leaves.)*

## Damage, connectivity & fracture (M8.3)

*(pending — the contact-impulse + explicit damage ops, the union-find support solve, the body swap,
and the cross-worker determinism proof.)*

## Event fan-out (M8.4)

*(pending — the `EventChannel<T>`, the VFX dust stub, the `engine/audio` null seam.)*

## Lifetime & budgets (M8.5)

*(pending — debris lifecycle, `WorldStats`-driven budgets, hull/compound `unregister`.)*
