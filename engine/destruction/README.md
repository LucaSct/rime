# rime::destruction — part-based destruction runtime (M8)

The headline system ([ADR-0029](../../docs/adr/0029-destruction-model.md)): it turns a cooked
**fracture pattern** (a `rime::assets` Destructible — the M8.1 cook) into standing, breakable physics.
A destructible is not a special-case simulation — it is a *consumer of the rigid-body core*, built to
be its first real customer. See [`docs/design/destruction.md`](../../docs/design/destruction.md) for
the systems reasoning and [ADR-0029](../../docs/adr/0029-destruction-model.md) for the model.

## The idea

- **Pattern** — a cooked destructible registered **once** into a `PhysicsWorld`: each convex part a
  `register_hull` ([ADR-0027](../../docs/adr/0027-convex-hull-shapes.md)), the whole a
  `register_compound` ([ADR-0028](../../docs/adr/0028-compound-shapes.md)). One registration, many
  instances — the shape economy.
- **Instance** — a standing destructible: **one static compound body** (the intact wall) plus per-part
  runtime state (health, alive bits). A bound-but-untouched instance is `Static`, so it costs the
  simulation nothing until something hits it.
- **Fracture** (M8.3) — damage accumulates into per-part health; a connectivity solve over the live
  **bonds** finds parts no longer supported by an **anchor**; the fracture is a body-swap that detaches
  the unsupported parts into debris. This module owns its physics bodies directly (not via
  `PhysicsSync`), because the ECS `Collider` cannot name a hull/compound id (ADR-0029 §6).

## Status (built brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| brick | what | state |
|-------|------|-------|
| M8.0 | the model — [ADR-0029](../../docs/adr/0029-destruction-model.md) | landed |
| M8.1 | the **fracture cook** (Rust `tools/asset-pipeline`) + the `Destructible` RMA1 asset | landed |
| M8.2 | **`DestructionWorld`** — register a pattern (hulls + compound), spawn static-compound instances, per-part state; the reflected `Destructible` component | landed |
| M8.3 | **damage → connectivity → detach** — the fracture body-swap (the hard core) | landed |
| M8.4 | health-transition **event fan-out** (`core::EventChannel`) + VFX dust stub + `engine/audio` seam | landed |
| M8.5 | **lifetime** — settled debris freeze/linger + a live-body cap (`lifecycle.cpp`) over the physics hull/compound `unregister` | landed |
| M8.6 | the **proof** — `samples/10-destructible-wall`, headless self-check + per-part render leaves | landed |
| M11.4a | the **bind path** (`bind.hpp` — the system M8.2 deferred), the public `DamageOp` + `committed_ops`/`apply_remote_ops`, `Authority`, and the state-application seam | landed |

### Scope note (M8.2)

M8.2 is **GPU-free** — it stands a destructible as physics and exposes each part's world placement
(`part_placement`), but does not yet create the **per-part render-leaf entities** ADR-0029 §5
ratified, nor the intact-wall pixel proof. Those land with the **M8.6 sample**, where a device and a
render path exist; the physics proof here (a raycast hits the intact wall exactly where a plain static
box's face would be) is the stronger *correctness* check that the compound stands right, and it runs on
every CI OS + the sanitizers with no GPU.

## Layout

```
engine/destruction/
├── include/rime/destruction/
│   ├── world.hpp       # DestructionWorld: register_pattern, spawn, damage/update, queries, events()
│   ├── events.hpp      # the DestructionEvent stream (M8.4): PartDamaged/Died/IslandDetached/Settled
│   ├── ids.hpp         # PatternId / InstanceId / kInvalidPartIndex (shared by world.hpp + events.hpp)
│   ├── damage_op.hpp   # the public DamageOp (M11.4) — the replicated unit, ADR-0033 A1
│   ├── bind.hpp        # the ECS bind system (M11.4): a Destructible entity becomes an instance
│   └── components.hpp  # the reflected Destructible ECS component (authoring intent)
└── src/
    ├── world.cpp       # the pImpl: pattern + instance tables (append-only in v1); load, stand, bind
    ├── damage.cpp      # damage → connectivity → the fracture body-swap, and the event fan-out
    ├── bind.cpp        # the bind pass + the instance→entity table the networking layer needs
    └── world_impl.hpp  # the shared internals (Pattern/Instance/Debris tables, the EventChannel)
```

## Networked destruction (M11.4)

Destruction knows nothing about networking — `rime::destruction_net` sits above both this module and
`rime::replication` — but three seams here exist for it, and they are worth knowing about:

- **`bind.hpp`** turns an entity carrying `Destructible{asset}` into a standing instance and records
  the link in `DestructibleInstanceRef`. This is the M8.2 "full ECS bind system" that was deferred to
  the sample; replication is what finally needed it, because the ENTITY (and so its NetId) is the
  only name for a destructible two peers can share. `DestructibleInstanceRef` stays **unreflected**
  for the same reason: it is a local table position, and replicating it would hand a client the
  server's index for its own.
- **`committed_ops()` / `apply_remote_ops()`** expose and accept the canonical damage-op list — the
  deterministic function's input (ADR-0029 §3), and therefore the thing that crosses the wire rather
  than the `apply_damage` calls that produced it (ADR-0033 A1).
- **`Authority` and the state-application seam** (`apply_detach_set`, `set_debris_state`): a mirror
  refuses locally-authored damage and skips the contact→damage drain, and a late-join or a drifted
  client can be handed *state* rather than only the events that produced it (ADR-0033 A3). The
  correction replays the ordinary body swap, so it reaches the same alive bits, the same remainder
  compound, and the same debris roster a peer that watched the collapse would have.

## Events (M8.4)

`update()` publishes a canonical `DestructionEvent` stream through a `core::EventChannel`, read after
the tick via `events()`: **PartDamaged / PartDied / IslandDetached / DebrisSettled**, each carrying a
world-space AABB (the M10-C2 hook). It is the fan-out seam — the `engine/vfx` dust stub, the
`engine/audio` null backend, and gameplay each read the one immutable span, none known to destruction
(remove any and the others are byte-identical — guardrail 2). The dust's actual GPU draw + pixel proof
land with the M8.6 sample; see [`docs/design/destruction.md`](../../docs/design/destruction.md).

## Building & testing

Built as part of the engine (`scripts/build.sh`). The tests are pure-CPU (they cook nothing — they
load the committed `wall.rdest` fixture and register it into a real `PhysicsWorld`) and run on every CI
OS plus ASan/UBSan and TSan:

```bash
ctest --preset dev -R rime_destruction_tests
```
