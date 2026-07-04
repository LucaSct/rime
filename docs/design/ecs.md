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

## M4.2–M4.3 — storage and queries (landed)

Between M4.1 and here the storage model of ADR-0018 became real. An allocator-backed `ChunkPool`
hands out 16 KiB blocks; a per-signature `ChunkLayout` places one SoA column per component (plus the
Entity column) inside a chunk; an `Archetype` groups all entities of one signature across a list of
chunks, keeping every chunk but the last full so a position is a compact `(chunk, row)`; and
add/remove-component performs the **archetype move** (relocate the shared components to the new
signature's archetype, swap-remove from the old). A `Query<Ts...>` (M4.3) then finds every archetype
whose signature ⊇ `{Ts…}` and scans their chunks **column-wise** — the packed, per-row-lookup-free
iteration the whole model exists for.

## M4.4 — running systems in parallel (M4.4a–c landed)

A *system* is a query's body run over the matching entities. M4.4a runs one system across all cores
(data parallelism); M4.4b runs independent systems side by side (task parallelism). The two stack.

### M4.4a — `par_for_each`: one system across the cores

`Query<Ts...>::par_for_each(jobs, body[, grain])` is the parallel twin of `for_each`. Its key idea is
the **parallel grain: one chunk per task**. Because every chunk is a *separate* pooled buffer
(ADR-0018), handing whole chunks to different workers guarantees no two tasks ever touch the same
cache line — data-parallelism with **no false sharing and no locks**, the payoff ADR-0018 designed the
chunk grain to deliver. The implementation flattens every matching chunk (across every matching
archetype) into one list and issues a **single** `core::JobSystem::parallel_for` over it, so the job
system load-balances the entire query at once instead of joining once per archetype. `grain` (chunks
per task, default 1) is the tuning knob for when a body is so cheap that per-task overhead would
dominate — M4.6 measures and picks it.

The contract is the discipline of any data-parallel loop. The body runs concurrently on **disjoint
rows**, so writing through the component references it is handed is always race-free (different
chunks ⇒ different memory). State the body *shares* across invocations is the caller's to synchronize,
exactly as for `parallel_for`. And the structural-change rule tightens: a parallel body must not
add/remove components or spawn/despawn — that would restructure the very archetypes being scanned.
Batched, deferred structural edits are the job of a later brick (**M4.4c**).

This is the engine's *first real multicore load* on the M1.6 Chase-Lev deque, so the Phase 0
ThreadSanitizer CI job now builds and runs `rime_ecs_tests` alongside `rime_core_tests` — the net that
keeps the parallel path race-free on every push (`tests/ecs/parallel_query_test.cpp`).

### M4.4b — the scheduler: independent systems side by side

`par_for_each` parallelizes *within* one system. The scheduler parallelizes *across* systems: given a
list of systems, which may run at once? The answer is declared on each system as an **access set** —
the components it reads and the components it writes (`System`, `SystemAccess` in `system.hpp`). Two
systems **conflict** iff one writes a component the other reads or writes; read-read overlap is safe.
That single rule carries the whole safety argument: if two systems don't conflict, then every location
one writes is one the other never touches, so their column writes land in disjoint memory and running
them concurrently is race-free — *even when they iterate the same archetype's chunks* (each writes a
different column). The flip side is a contract the scheduler cannot check: a system must declare its
access truthfully (the same discipline Unity DOTS and Bevy require).

`Schedule` batches the systems into **phases** by *ASAP leveling* of the conflict order — a system is
placed one phase past the last earlier system it conflicts with (`phase(j) = 1 + max{ phase(i) : i<j,
i ⨯ j }`, else 0). Two properties fall out of that one line: no two systems in a phase conflict (so the
phase runs concurrently), and conflicting systems keep their declared order (a writer's phase precedes
its reader's). The phase count is exactly the longest chain of mutually-conflicting systems — the
irreducible serialization the declared hazards force; everything else runs in parallel.

Execution mirrors that structure: phases run in sequence, and the join between them is the barrier that
publishes one phase's writes to the next. Within a phase each system is submitted as a job and they run
side by side (a lone system in its phase is run inline — no needless fork/join). A system body may
itself call `par_for_each`, so a scheduled run nests task parallelism over data parallelism — a
submit/wait from within a running job, which the job system's *participating* `wait()` handles without
deadlock. This concurrent path is a second multicore load, so `tests/ecs/schedule_test.cpp` (two
independent systems writing disjoint columns of one shared 20k-entity archetype) rides the same TSan
CI net. The structural-change rule still holds inside a phase, but M4.4c below lifts it with deferred
edits — a system records structural changes and the scheduler replays them at the phase boundary.

### M4.4c — deferred structural changes: restructuring safely

A system may not spawn/despawn or add/remove a component *directly* mid-phase — that moves entities
between archetypes while other systems (or its own `par_for_each`) scan them. A `CommandBuffer`
(`command_buffer.hpp`) breaks the deadlock: the body **records the intent** — `spawn`, `despawn`,
`add_component`, `remove_component` — and `apply(World&)` **replays** it later, at a safe point. Each
command is captured as a small closure over the typed `World` call, so the buffer needs no per-type
erasure machinery and `apply` is just "run them in order"; the closure remembers the component type.

Recording is **thread-safe** (a mutex guards the record list), because the natural place to record is
*inside* `par_for_each` — "despawn every entity whose health hit zero" runs on many workers at once,
all pushing into one buffer. Structural change is the rare, bursty path (ADR-0018), so the lock is
uncontended in practice. The `Schedule` gives each system in a phase its **own** buffer (so concurrent
systems never share one), and applies them — in system order — at the join that ends the phase. That
join is exactly the safe point: no system is iterating, and it is also the barrier that makes the
edits visible to the next phase. `tests/ecs/command_buffer_test.cpp` proves each op takes effect only
on apply, that recording 10k despawns concurrently from `par_for_each` is race-free (TSan), and that
the schedule applies a reaper system's despawns at its phase boundary.

(Order note: commands replay in record order, which under parallel recording is unspecified *across*
threads — so record commutative edits, the usual case. A future extension can reserve entity ids at
record time so a deferred `spawn` returns a usable handle immediately; M4 doesn't need it.)

## M4.5 — the transform hierarchy (landed)

The first real *consumer-facing* system: parent/child placement composed into world space. Three
components — `LocalTransform` (placement relative to the parent), `WorldTransform` (the absolute
placement render/physics read), `Parent` (the entity hung off; absent or `kNullEntity` = a root) — and
one pass, `propagate_transforms(world, jobs)`, computing `world = parent.world * local` for children
and `world = local` for roots (`core::Transform::operator*` is the compose). Full derivation, including
the parallelism argument, in [docs/math/transform-hierarchy.md](../math/transform-hierarchy.md).

The one subtlety is **order**: a child must compose against its parent's *already-updated* world, so
the pass processes the hierarchy **depth by depth** (roots first), and each depth level — whose members
are mutually independent, writing their own world and only reading their parents' at a shallower,
finished level — updates with a single `parallel_for`. The join between levels is the barrier that
publishes a parent's world to its children. The common case, a flat scene with no parents, is all
roots and takes a fully-parallel `world = local` fast path — the shape of the M4.6 proof.

We recompute every world transform each call. ADR-0018's per-chunk change-detection stamps were meant
to let this pass **skip subtrees whose locals didn't move** (dirty propagation); we defer that until a
profile shows it matters (measure before optimize) — recompute-all is correct and trivially parallel,
and change detection's real consumers (M9 editor sync, M11 replication) come later. The seam is
designed; the optimization waits.

## What's next

- **M4.6** the proof: `samples/05-ecs-playground`, 100k+ entities updating in parallel with transforms
  composing correctly, measured — and uncomment `engine/ecs` in the root `CMakeLists.txt`. Completes
  M4's "done when".
- **Change detection** (deferred) — the ADR-0018 per-column version stamps + dirty-subtree skipping,
  to land when a profile or its editor/networking consumers call for it.
