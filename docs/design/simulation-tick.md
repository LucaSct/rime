# The simulation tick — order, sync, and change detection (M7.6)

Status: v1, 2026-07-14. Ties together ADR-0023 (the fixed tick), ADR-0018 §4 (change
detection), and ADR-0026 (physics determinism). The code seams are `rime::app::Application` /
`FixedTimestep` (app), `rime::ecs::World` / `Query` / `Schedule` (ecs), and
`rime::physics::PhysicsSync` / `PhysicsWorld` (physics).

This note is the canonical answer to three questions that only make sense together:

1. **When** does the world advance — and why a *fixed* step?
2. **In what order** do gameplay, transforms, and physics run inside one step, and where does the
   ECS↔physics bridge sit?
3. **How** does a consumer (GPU upload, editor sync, netcode) process *only what changed* this
   step instead of re-scanning the whole world?

---

## 1. The fixed tick (ADR-0023)

The render frame runs as fast as it can; the simulation must not. If the world advanced by a
variable per-frame `dt`, its state after some wall-clock second would depend on how that second
happened to be chopped into frames — fatal for replay, lockstep netcode (M11), and reproducing a
bug. So the sim advances only in equal `fixed_dt` steps, driven by a time accumulator
(`fixed_timestep.hpp`, the standard "Fix Your Timestep" pattern):

```
accumulator += frame_dt;
while (accumulator >= fixed_dt) { tick(); accumulator -= fixed_dt; }   // 0..N ticks this frame
alpha = accumulator / fixed_dt;                                        // [0,1) render interpolation
```

The payoff is a hard invariant: **the world state after `k` ticks is a pure function of `k`** (and
the initial state and `fixed_dt`) — independent of frame pacing. Everything below is about keeping
that purity while wiring three modules together.

`Application` owns the accumulator, the `JobSystem`, the ECS `World`, and the sim `Schedule`.
Physics is deliberately **not** owned by `Application` in v1 — `PhysicsSync` is a standalone bridge a
game or sample composes into its tick (the module layering keeps `app` free of a `physics`
dependency; wiring a live physics step into `Application` is a later brick, alongside the physics
playground sample).

## 2. The canonical per-tick order

One tick, top to bottom. Steps 1–3 are the ECS half; 4–6 are the physics bridge; 7 is the consumer.

| # | Step | Who | Notes |
|---|------|-----|-------|
| 1 | **Open a change epoch** — `world.advance_version()` | `Schedule::run` (or a hand-rolled tick) | Everything written this tick stamps a version strictly *after* any consumer's last checkpoint. |
| 2 | **Gameplay / input systems** | `Schedule` systems | Read input, write intent (velocities, spawn/despawn via command buffers). Mutating `get<T>()` requires `mark_changed<T>()` (see §3). |
| 3 | **Transform propagation** — `propagate_transforms` | ecs | Compose `LocalTransform → WorldTransform` down the parent chain (parents before children). Physics reads the resulting `WorldTransform`. |
| 4 | **Reconcile** — `PhysicsSync::reconcile` | physics bridge | *Bind* new intent entities (RigidBody + Collider + WorldTransform, no handle yet) to freshly-created bodies placed at their transform; *unbind* bodies whose entity died or dropped its intent. **Structural** (adds/removes `RigidBodyHandle`), so it runs between phases on the main thread, never concurrently with a query over those archetypes. |
| 5 | **Step** — `physics.step(fixed_dt)` | physics | The deterministic sim: integrate → broadphase → narrowphase → islands → solve (parallel) → integrate positions → NGS. Bit-identical for any worker count (`world_hash()`, ADR-0026). |
| 6 | **Write-back** — `PhysicsSync::write_back` | physics bridge | Each **awake** dynamic body's pose → its entity's `WorldTransform`, stamped changed at the current version. Asleep and static/kinematic bodies are skipped — this is where sleeping meets change detection (§4). |
| 7 | **Consume deltas** | render / editor / net | `query<…>().for_each_changed(checkpoint, …)` visits only chunks written since `checkpoint`; then remember `world.version()` as the next checkpoint. |

`PhysicsSync::step(world, physics, dt)` is sugar for steps 4→5→6 in order — the common body of a
tick. The caller still owns step 1 (advancing the version), because the version boundary is a
property of the *tick*, not of physics.

## 3. Change detection (ADR-0018 §4)

The mechanism is three small pieces:

- **A monotonic world version.** `World` holds a 64-bit counter, starting at 1 (so `changed_since(0)`
  matches everything ever written). `advance_version()` bumps it once per tick. 64 bits never wraps
  in practice — centuries at 60 Hz — which sidesteps the modular-comparison dance a 32-bit tick
  counter would force.
- **Per-column write stamps.** Every chunk records, for each component *column*, the version at
  which that column was last written. Adding/removing a component (a structural move) stamps every
  column of the destination chunk; overwriting in place stamps that column; and a system that
  mutates data through `get<T>()` reports it with `world.mark_changed<T>(e)` — the **writer
  discipline** the ADR asks of anything that bypasses a structural change.
- **A skip test on the query.** `for_each_changed(since, f)` scans a matching chunk only if one of
  the query's columns on it was written after `since`; otherwise the whole chunk is skipped.
  `par_for_each_changed` is the same filter, then one job per surviving chunk.

**The grain is the chunk column, not the row.** A chunk with *any* recent write to a queried column
is scanned in full — so `for_each_changed` is a *conservative* filter: it never misses a change, but
it may hand you a few unchanged neighbours that share a changed chunk. That is the right trade for
an archetype ECS (the chunk is the unit of contiguous storage and of parallelism); a consumer that
needs exactness re-checks per row. Grouping entities that change together into the same archetype
(and thus chunk) is what makes the filter tight in practice.

## 4. Why sleeping and change detection are the same win

M7.5 lets a resting island *sleep*: it is skipped by integration and the solver until something
wakes it. M7.6's write-back skips asleep bodies. Compose them and a **settled world stamps
nothing** — `write_back` marks no column, so `for_each_changed` finds no chunk to scan, so the GPU
uploads nothing, the editor syncs nothing, and replication sends no delta. Work becomes proportional
to what actually *moved*, not to world size. A single body dropped into a sleeping stack wakes only
its island, so only those chunks re-enter the change set. This is the property the physics core was
shaped around, and `sync_test.cpp` pins it directly: after a box sleeps, a further tick reports zero
changed transforms; waking it brings back exactly its chunk.

## 5. Determinism through the bridge

The bridge adds no nondeterminism: `reconcile` binds in a fixed order (roster scan, then a query
scan), `step` is bit-identical across thread counts by construction (ADR-0026), and `write_back`
touches each body independently. So the tick as a whole stays a pure function of `(state, k,
fixed_dt)` — driving the same scene through `PhysicsSync::step` twice yields an identical
`world_hash()` *and* identical entity transforms (a `sync_test.cpp` case). `world_hash()` remains the
witness the parallel step and replay validation both build on.

## 6. Scale, kinematics, and what v1 defers

- **Scale is the game's; physics owns place.** A simulated body's `WorldTransform` has its
  translation and rotation written by physics each tick; its **scale is never touched**. Physics
  ignores scale entirely in v1 (a body's collision extents come from its `Collider`, not the
  transform) — documented loudly because it is a real limitation, not an accident.
- **Kinematic push-in is deferred.** v1 treats a simulated body's `WorldTransform` as
  physics-owned (physics → ECS). Driving a kinematic body *from* a game-written `WorldTransform`
  (ECS → physics, pre-step) is the natural next seam; `reconcile`/`write_back` are structured to
  grow it without a redesign.
- **Physics → `LocalTransform` for parented bodies is deferred.** Write-back targets
  `WorldTransform` (what physics simulates in). A simulated body that is also a child in the
  transform hierarchy would need its world pose decomposed back into a local one; v1 assumes
  simulated bodies are effectively roots.
- **Render interpolation** uses the accumulator's `alpha` (ADR-0023 §3); wiring physics states into
  it is a measured later need, not this brick.

---

### See also

- ADR-0023 — the fixed-tick loop and the interpolation seam.
- ADR-0018 §4 — the ECS storage model and the change-detection contract.
- ADR-0026 — the physics core and its determinism guarantee.
- `docs/design/physics.md` §"Change detection & ECS sync" — the physics-side write-up.
