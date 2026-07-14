# engine/physics — the rigid-body core

`rime::physics` is Rime's **own rigid-body physics engine** — not a wrapper around a third-party
solver. The whole algorithm suite (broadphase, GJK/EPA narrowphase, a sequential-impulse solver,
islands + sleeping, scene queries) is grown here so the code *teaches* and is shaped for the
destruction system that sits on top (M8). Building our own was decided in
[ADR-0026](../../docs/adr/0026-physics-core.md) — VISION principle #1 (power beats portability) and
#3 (the codebase is a textbook); Jolt was studied, not integrated. The design is written up in
[../../docs/design/physics.md](../../docs/design/physics.md); the math derivations live in
[../../docs/math/](../../docs/math/) (`rigid-body-dynamics.md`, `gjk-epa.md`,
`sequential-impulse.md`).

**GPU-free by construction.** Collision geometry enters as shape descriptions and poses, never GPU
resources, so every proof runs on CI + the sanitizers (lavapipe needs no involvement). The module
depends only on `core` (math, handles, allocators, the job system) and `ecs` (the components live
here); **nothing depends on its internals** — the broadphase/narrowphase/solver live under `src/`
(PRIVATE), invisible above the `PhysicsWorld` seam, the same RHI-style discipline the renderer uses.
Destruction (M8), lighting invalidation (M10), and the effects/fluids tracks depend on
`rime::physics`, never the reverse.

**Determinism is a contract, not an accident.** One `step()` is bit-identical run to run and across
any worker-thread count (`world_hash()` is the witness) — the property networked destruction (M11)
and replay validation build on. Scope: same-binary reproducibility, *not* cross-platform lockstep
(ADR-0026); no `-ffast-math`.

## Status (built bottom-up, brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| Brick | Provides | State |
| --- | --- | --- |
| M7.0 | physics-core decision — own core, the algorithm suite, determinism scope ([ADR-0026](../../docs/adr/0026-physics-core.md)) | landed |
| M7.1 | the `PhysicsWorld` seam; SoA `BodyPool` (generational ids, swap-remove); `RigidBody`/`Collider` reflected components; semi-implicit-Euler + quaternion integration | landed |
| M7.2 | **broadphase** — dual dynamic AABB trees (static/dynamic), fat AABBs, the canonical deterministic pair list | landed |
| M7.3 | **narrowphase** — GJK + EPA + reference-face clipping → manifolds; sphere/capsule fast paths; feature-id warm-start cache | landed |
| M7.4 | **solver** — sequential-impulse PGS, warm starting, friction pyramid, restitution, an NGS position pass (not Baumgarte) | landed |
| M7.5 | **islands + sleeping + the parallel step** on `core::JobSystem` — bit-identical across thread counts, TSan-netted | landed |
| M7.6 | **fixed-tick ECS sync** (`PhysicsSync`) + **change detection** (ADR-0018 §4); awake-only write-back | landed |
| M7.7 | **scene queries** — raycast + `overlap_sphere` through the BVH, motion-class filters; **external impulses** (`apply_impulse`) | landed |
| M7.8 | **the proof** — `samples/09-physics-playground` (headless self-check, M7's "done when") + the `Application` per-tick hook | landed |
| M7.9 | **contact & sleep events** — began/persisted/ended contacts with point + normal + impulse, `Slept`/`Woke`; buffered, double-buffered, canonical per-tick order (the M8-damage input) | landed |

**M7's "done when" (ROADMAP): objects fall/collide/stack; raycasts hit; runs parallel to the frame —
met**, proven by `samples/09-physics-playground` self-checking in CI. M7.9 is the first fast-follow
into M8's runway.

### Deferred (planned, not yet built — fast-follows into M8's runway)

- **Trigger/sensor events** — the third of the M7.9 "contact/trigger/sleep" brick, held back: a
  sensor body (overlaps but generates no contact response) is not in M7's shipped body scope and has
  no consumer yet (M8 damage rides contact events). Lands with the first gameplay volume; it reuses
  the existing overlap machinery.
- **Shapes II** — runtime convex hull, polyhedral mass properties, static triangle mesh + midphase,
  compound shapes.
- **CCD** (speculative contacts) + debris-scale tuning + a `WorldStats`/stress harness.

## Layout

```
engine/physics/
├── include/rime/physics/   # the seam — everything the rest of the engine may include
│   ├── physics.hpp         #   umbrella (include this, or a finer header below)
│   ├── world.hpp           #   PhysicsWorld: create/destroy bodies, step, queries, impulses
│   ├── body.hpp            #   BodyId, MotionType, BodyDesc, BodyState
│   ├── shape.hpp           #   ShapeType, ShapeDesc, analytic mass properties
│   ├── aabb.hpp            #   Aabb + overlap / ray-slab helpers
│   ├── contact.hpp         #   ContactPoint, Manifold (narrowphase out → solver in)
│   ├── events.hpp          #   ContactEvent, SleepEvent — step()'s per-tick event report (M8 input)
│   ├── query.hpp           #   Ray, RayHit, QueryFilter (scene queries)
│   ├── components.hpp      #   RigidBody / Collider / RigidBodyHandle ECS components
│   └── sync.hpp            #   PhysicsSync — the ECS ↔ PhysicsWorld bridge
└── src/                    # PRIVATE internals — invisible above the seam
    ├── world.cpp           #   the pImpl: body pool, step pipeline, queries, impulses
    ├── aabb_tree.hpp       #   the dynamic AABB tree (broadphase + ray/overlap engine)
    ├── support.hpp/gjk.hpp/epa.hpp/narrowphase.hpp   # the collision algorithm suite
    ├── solver.hpp          #   sequential-impulse + NGS
    ├── islands.hpp         #   union-find island partition
    └── scene_query.hpp     #   exact ray-vs-shape / sphere-vs-shape geometry
```

## Building & testing

Built as part of the engine (`scripts/build.sh`). The tests are pure-CPU and run on every CI OS plus
ASan/UBSan and TSan (the parallel step is the threading surface):

```bash
ctest --preset dev -R rime_physics_tests      # the unit suite
build/dev/bin/physics_playground --verbose    # M7's "done when" self-check, with a report
```
