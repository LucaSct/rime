# engine/ecs — the data-oriented world

`rime::ecs` is Rime's **Entity-Component-System**: the world modeled as *data laid out for the
cache*, not a tree of objects. Entities are ids; components are plain data stored in tight per-type
arrays; systems are functions that run over matching component sets, in parallel on the job system.
This is how the engine gets both performance and a model that is easy to reason about and extend
(see [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md)).

The storage model — **archetype/SoA chunked tables**, generational-`Handle` entities, chunks drawn
from `core`'s allocators, and change detection built in — is decided in
[ADR-0018](../../docs/adr/0018-ecs-storage-model.md); the design is written up in
[../../docs/design/ecs.md](../../docs/design/ecs.md). Depends only on `core` (the layer below):
`Entity` is a `core::Handle`, and component registration captures `core`'s reflection.

## Status (built bottom-up, brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| Brick | Provides | State |
| --- | --- | --- |
| M4.0 | storage-model decision (archetype/SoA chunks; change detection) — [ADR-0018](../../docs/adr/0018-ecs-storage-model.md) | landed |
| M4.1 | `engine/ecs` seam; **entity directory** (generational spawn/despawn/liveness/recycling); **component registry** (reflection-aware) behind the `World` | landed |
| M4.2a | archetype **storage primitives** — allocator-backed `ChunkPool`, per-signature `ChunkLayout`, `Chunk` SoA row store (swap-remove) | landed |
| M4.2b | World integration — `Archetype` keyed by `ComponentSignature`; `spawn_with` / `add` / `remove` component = archetype move; `get`/`has`; directory `location` wired | landed |
| M4.3 | **`Query<Ts...>`** — column-wise iteration over the entities that have a given component set | landed |
| M4.4a | **`Query::par_for_each`** — the query body run across all cores on the `JobSystem`, one chunk per task (no false sharing); TSan CI job extended over `rime_ecs_tests` | landed |
| M4.4b | **`System` + `Schedule`** — declared read/write **access sets** batched into parallel **phases** (independent systems run concurrently, conflicting ones keep order) | landed |
| M4.4c | **`CommandBuffer`** — record structural edits (spawn/despawn/add/remove) inside a system (thread-safe under `par_for_each`); the schedule applies them at each phase boundary | landed |
| M4.5 | transform hierarchy (`core::Transform` composition; change-detection's first consumer) | planned |
| M4.6 | proof `samples/05-ecs-playground` — 100k+ entities in parallel, transforms composing | planned |

## Layout

```
include/rime/ecs.hpp        # umbrella header
include/rime/ecs/
    entity.hpp              # Entity = core::Handle<EntityTag> (a generational id)
    entity_directory.hpp    # the flat, generational index → location table
    component.hpp           # ComponentId + type-erased ops + the reflection-aware registry
    signature.hpp           # ComponentSignature — the sorted set of ids identifying an archetype
    chunk_pool.hpp          # allocator-backed 16 KiB chunk blocks (core::PoolAllocator, load-bearing)
    chunk.hpp               # per-signature SoA ChunkLayout + the Chunk row store (swap-remove)
    archetype.hpp           # an archetype's chunks; insert / component access / archetype-move removal
    query.hpp               # Query<Ts...> — find matching archetypes, scan their columns; par_for_each
    system.hpp              # System + SystemAccess (read/write sets) + signature_of<Ts...> helper
    schedule.hpp            # Schedule — batch systems into parallel phases, run them on the JobSystem
    command_buffer.hpp      # CommandBuffer — record structural edits, apply them at a safe point
    world.hpp               # the World front door: entities, component types, and the archetypes
src/
    entity_directory.cpp · signature.cpp · chunk_pool.cpp · chunk.cpp · archetype.cpp · world.cpp · schedule.cpp
```

## Using it

```cpp
#include "rime/ecs.hpp"
using namespace rime::ecs;

struct Position { float x, y, z; };
struct Velocity { float dx, dy, dz; };

World world;
const Entity e = world.spawn_with(Position{0, 0, 0}, Velocity{1, 0, 0});
world.has<Position>(e);                         // true
world.get<Velocity>(e)->dx;                     // 1 — resolved through the entity's archetype

world.add_component<Health>(e, Health{100});    // archetype move: e relocates, keeps Position+Velocity
world.remove_component<Velocity>(e);            // another move; Position + Health preserved

// Iterate every entity that has all of Ts, column-wise across matching archetypes:
world.query<Position, Velocity>().for_each([](Position& p, Velocity& v) { p.x += v.dx; });

// Same iteration, across all cores — one chunk per job, no false sharing (M4.4a):
rime::core::JobSystem jobs;
world.query<Position, Velocity>().par_for_each(jobs, [](Position& p, Velocity& v) { p.x += v.dx; });

// A system declares what it reads/writes; the Schedule runs non-conflicting systems in parallel and
// orders conflicting ones (M4.4b):
Schedule schedule;
schedule.add({"move", {/*reads*/ signature_of<Velocity>(world), /*writes*/ signature_of<Position>(world)},
             [](World& w, rime::core::JobSystem& j, CommandBuffer& cmd) {
                 w.query<Position, Velocity>().par_for_each(j, [](Position& p, Velocity& v) { p.x += v.dx; });
                 // ...and record any structural changes to be applied at the phase boundary:
                 // w.query<Health>().par_for_each(j, [&](Entity e, Health& h){ if (h.hp<=0) cmd.despawn(e); });
             }});
schedule.run(world, jobs);   // phases run in order; non-conflicting systems run side by side

world.despawn(e);                               // tears the row out; a swapped entity is fixed up
world.get<Position>(e);                         // nullptr — stale entity, safe no-op
```

Components are plain, trivially-copyable structs (ADR-0018). Reflect one with `RIME_REFLECT_*` and
its fields are captured at registration — so it is serializable now and editor-inspectable later.
