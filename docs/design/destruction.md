# Destruction — design note (M8)

Companion to `engine/destruction` (M8) and the fracture cook in `tools/asset-pipeline`. Like
[`physics.md`](physics.md), this is the running design record — it grows a section per brick, in the
order the bricks land. The *decisions* live in [ADR-0029](../adr/0029-destruction-model.md) (and the
physics substrate in [ADR-0026](../adr/0026-physics-core.md)/[0027](../adr/0027-convex-hull-shapes.md)/
[0028](../adr/0028-compound-shapes.md)); this note covers the *systems* reasoning — the pipeline, the
data, and the trade-offs — and the math derivations get their own notes as they land.

Current coverage: the model (M8.0, this note's overview + ADR-0029), the fracture cook (M8.1), and the
load/stand/bind runtime (M8.2). Damage/connectivity/fracture (M8.3), the event fan-out (M8.4),
lifetime/budgets (M8.5), and the proof sample (M8.6) each add a section as they ship.

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

The cook is offline tooling (Rust, `tools/asset-pipeline`) that turns a convex source — v1: a box
(wall, column, slab) — into the `Destructible` RMA1 asset the runtime loads. The partition is a
**seeded Voronoi diagram** clipped to the box: each cell is the intersection of half-spaces (the six
box faces plus one bisector per other site), so every part is **convex by construction** — no
quickhull needed to *generate* it (quickhull's robustness role only appears when a future brick
fractures a non-convex mesh). Cell vertices come from plane-triple intersections kept inside every
half-space; each face is the CCW loop of the vertices on its plane — exactly the CSR shape
`register_hull` validates. Volume and COM follow from the divergence theorem, so a part's mass fraction
is its volume fraction under uniform density; two cells sharing a face are **bonded** (strength ∝ the
shared area); a part touching the cook's anchor plane is an **anchor**. The full derivation is in
[`docs/math/voronoi-fracture.md`](../math/voronoi-fracture.md); the asset byte layout is in
[`assets.md`](assets.md). The done-when is the cross-language oracle: every cooked part registers into
a real `PhysicsWorld` (the physics validator *is* the cook's acceptance gate), with the summed hull
volumes matching the source box.

## The runtime: load, stand, bind (M8.2)

`engine/destruction` is born here — a removable feature module (guardrail 2) depending only on core,
ecs, physics, and assets. Its seam is `DestructionWorld`, and M8.2 is the "load, stand, bind" third of
destruction: a cooked pattern becomes standing physics. Damage and the fracture body-swap (M8.3), the
event fan-out (M8.4), and lifetime (M8.5) grow this same class.

### Patterns and instances: the shape economy

`register_pattern(asset, world)` is the **cold path**. It walks the cooked parts, `register_hull`s each
one (the cook already emitted COM-centred CSR geometry that passes the hull validator — the M8.1 oracle
proved it), and `register_compound`s the lot, each child the part's hull at its cooked COM (identity
rotation — an intact part is not rotated relative to its destructible). One asset → one `PatternId`,
holding the compound id, the per-part hull ids and COMs (kept for M8.3's fracture, which re-registers
surviving subsets), and the cooked bonds/anchors/damage material. If any hull or the compound is
rejected, the whole pattern is rejected with no partial registration — a malformed cook fails loudly.

`spawn(pattern, placement, world)` is the **hot path**, and it is deliberately cheap: one
`create_body` for a **static compound** at the placement, plus two small per-part vectors (health = 1,
alive = true). A pattern registers once; a wall of a hundred instances shares that one hull set and
compound — the ADR-0027/0028 shape economy, realized. The intact body being *static* is what makes a
standing destructible free: it never integrates or solves, so `WorldStats` over a scene of untouched
walls reads zero awake bodies and zero active islands (the M8.2 "≈ static baseline" proof). It
participates in collision only when something hits it — which is exactly how M8.3 will hear damage.

### Per-part state, and why it is here now

Each instance carries a per-part `health`/`alive` SoA. On M8.2 it is inert (everything alive, full
health) — the point is to settle the **state model and its accessors** (`part_alive`, `part_health`,
`part_placement`) before M8.3's damage logic stands on them, the same reader-before-writer discipline
the asset formats followed. `part_placement` carries a part's cooked COM through the instance
transform — the hook a per-part render leaf will draw at (M8.6).

### What M8.2 defers, honestly

The ADR-0029 §5 **render-leaf entities** (and the intact-wall pixel proof) are *not* built here: M8.2
is GPU-free, so it runs on every CI OS and under the sanitizers with no device. The physics proof — a
raycast hits the intact wall's face exactly where a plain static box's would (x = +1.0 for a 2 m wall)
— is the stronger *correctness* statement that the compound stands right; the visual binding lands with
the M8.6 sample, where a camera and render path exist. The ECS `Destructible` component is reflected
here (the authoring surface), but the full author-a-Destructible-entity → instance bind system also
waits for the sample; M8.2 drives spawning through the `DestructionWorld` API directly.

### Proofs

`tests/destruction/world_test.cpp`, pure CPU, loading the committed `wall.rdest` fixture (shared with
the assets oracle) into a real `PhysicsWorld`: the pattern's part/bond/anchor counts survive the round
trip; the intact wall stands as one live static body and a **raycast hits its +X face at x ≈ 1.0** (a
ray over the top misses); a stepped scene of the bound wall stays at **zero awake bodies / zero active
islands** and hashes identically across two runs (deterministic — the M11 contract); `part_placement`
matches `core::transform_point`; unknown ids are safe no-ops and a **degenerate part is rejected**; and
25 instances share one registered pattern (the shape economy, ASan-covered). `components_test.cpp`
proves the `Destructible` component reflects and registers. Green under dev + ASan/UBSan + TSan.

## Damage, connectivity & fracture (M8.3)

*(pending — the contact-impulse + explicit damage ops, the union-find support solve, the body swap,
and the cross-worker determinism proof.)*

## Event fan-out (M8.4)

*(pending — the `EventChannel<T>`, the VFX dust stub, the `engine/audio` null seam.)*

## Lifetime & budgets (M8.5)

*(pending — debris lifecycle, `WorldStats`-driven budgets, hull/compound `unregister`.)*
