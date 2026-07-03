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
| M4.2 | archetype / chunk storage; add/remove component = archetype move | planned |
| M4.3 | queries + chunk-wise iteration | planned |
| M4.4 | parallel system scheduler on the `JobSystem` | planned |
| M4.5 | transform hierarchy (`core::Transform` composition; change-detection's first consumer) | planned |
| M4.6 | proof `samples/05-ecs-playground` — 100k+ entities in parallel, transforms composing | planned |

## Layout

```
include/rime/ecs.hpp        # umbrella header
include/rime/ecs/
    entity.hpp              # Entity = core::Handle<EntityTag> (a generational id)
    entity_directory.hpp    # the flat, generational index → location table
    component.hpp           # ComponentId + type-erased ops + the reflection-aware registry
    world.hpp               # the World front door (entities + component types; storage grows here)
src/
    entity_directory.cpp    # the directory implementation (World is header-only for now)
```

## Using it (M4.1)

```cpp
#include "rime/ecs.hpp"
using namespace rime::ecs;

World world;
const Entity e = world.spawn();
world.is_alive(e);              // true
world.despawn(e);
world.is_alive(e);              // false — and a stale copy of `e` stays false forever (generation guard)

world.register_component<Position>();          // once; idempotent
const ComponentId id = world.component_id<Position>();
```

Components are plain, trivially-copyable structs (ADR-0018). Reflect one with `RIME_REFLECT_*` and
its fields are captured at registration — so it is serializable now and editor-inspectable later.
