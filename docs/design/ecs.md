# ECS — design note (M4)

Companion to `engine/ecs/`. The systems bricks ship *design notes* like this one — the reasoning
and trade-offs behind the code, so the repo stays legible enough to learn engine development from.
The **decision** this note builds on (archetype/SoA storage, chunks, generational entities, change
detection) is [ADR-0018](../adr/0018-ecs-storage-model.md); this note is the *how*, updated as the
M4 bricks land.

## What an ECS is, and why Rime is one

The world is modeled as **data laid out for the cache**, not a tree of objects:

- **Entities** are ids — nothing more.
- **Components** are plain data (`Position`, `Velocity`, `Mesh`) attached to entities.
- **Systems** are functions that run over every entity with a given set of components, ideally in
  parallel across cores.

This is the data-oriented core of the whole engine ([ARCHITECTURE.md](../ARCHITECTURE.md)): it is
what makes "update 100k entities every frame" both *fast* (linear scans over packed arrays) and
*parallel* (independent chunks handed to the job system). Rendering, physics, and destruction will
all keep their state as components in this world.

## The layout (target)

```
                     ┌──────────────────────────────────────────────┐
   Entity{index,gen} │  entity directory:  slots_[index] → location  │   (M4.1)
        │            └──────────────────────────────────────────────┘
        │                         location = { archetype, chunk, row }
        ▼
   archetype (one per distinct component signature, e.g. {Position, Velocity})   (M4.2)
        │
        ├─ chunk 0  (16 KiB)   columns:  [Position…Position][Velocity…Velocity]   ← SoA within a chunk
        ├─ chunk 1  (16 KiB)   …                                                    + per-column version
        └─ …                                                                          stamps (change detection)
```

A **query** for `{Position, Velocity}` (M4.3) finds every archetype whose signature is a superset,
and scans their chunks' columns linearly. A **system** (M4.4) is that scan run as `parallel_for` over
chunks on the `JobSystem` — one chunk per task, so no two threads touch the same cache line.

## M4.1 — entities and component registration (landed)

The foundation the storage stands on. Two pieces, both behind the `World` front door.

### Entities are generational handles

An `Entity` is a `core::Handle<EntityTag>` — a 32-bit directory `index` + 32-bit `generation`
(`entity.hpp`). It carries no data; it *names* one. The **entity directory** (`entity_directory.hpp`)
is a flat table indexed directly by `index`:

```
slots_[index] = { generation, alive, location }
```

- `allocate()` reuses a freed index (LIFO, cache-hot) or grows the table, returning `Entity{index,
  generation}`.
- `free()` bumps the slot's `generation` and returns the index to a free list. Any `Entity` still
  holding the old generation now mismatches → it is **detected as stale**, never silently aliasing
  the next entity to reuse that index. This is the same use-after-free defense the `SlotMap`
  (`core/containers/slot_map.hpp`) gives resource handles.

**Why a flat directory and not a `SlotMap<Location>`?** A `SlotMap` adds a slot→dense indirection to
give *dense iteration of live elements* — but the ECS never iterates the directory on the hot path;
it iterates **archetypes**. Directory access is always random ("is this entity alive? where does it
live?"), which wants a single direct index. So we take the SlotMap's generational-recycling idea and
drop its dense-array machinery. (See ADR-0018, "Entities as `SlotMap<Location>`" under Alternatives.)

The `location` field (`{archetype, chunk, row}`) is *reserved* in M4.1 and written by the archetype
layer in M4.3 — the honest state of this brick: the entity lifecycle and generational safety are real
and tested now; the pointer into storage lands with the storage.

### Components are registered once — through reflection

Before the type-erased storage can hold a component type `T`, it must be **registered**
(`component.hpp`), which yields a `ComponentInfo`:

- a dense **`ComponentId`** (0, 1, 2… in registration order) — the compact key that will index columns
  and build archetype signatures;
- `T`'s **size and alignment**;
- type-erased **`ComponentOps`** (`default_construct` / `relocate` / `destroy`) so a chunk can build
  and move rows during an archetype move (M4.3) without knowing `T`. For M4's trivially-copyable
  components (ADR-0018) `relocate` is a `memcpy` and `destroy` a no-op; the general branches are kept
  correct so the storage never changes when non-trivial components are eventually allowed;
- `T`'s **reflection `TypeInfo`** when `T` is reflected (`RIME_REFLECT`) — its field layout. This is
  the "register once ⇒ serializable now (M1.7), editor-inspectable later (M9)" bet: one registration
  serves storage, serialization, and the future editor. Reflection is optional (tags need none) but
  captured whenever present.

Type identity without RTTI uses the classic *address-of-a-static* trick: `&type_key<T>::key` is a
unique, stable key per `T`, so the registry maps types to ids with no `<typeinfo>` dependency.

## What's next

- **M4.2** archetype/chunk storage — the SoA columns and the archetype-move that add/remove-component
  performs; chunks drawn from `core`'s allocators (the module becomes load-bearing).
- **M4.3** queries + chunk-wise iteration, and the entity `location` finally wired.
- **M4.4** the parallel system scheduler on the `JobSystem`.
- **M4.5** the transform hierarchy — `core::Transform` composition and change-detection's first
  consumer (skip chunks whose locals didn't move).
- **M4.6** the proof: `samples/05-ecs-playground`, 100k+ entities updating in parallel with transforms
  composing, measured.
