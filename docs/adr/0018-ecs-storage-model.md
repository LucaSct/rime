# ADR-0018: ECS storage model — archetype/SoA chunked tables (M4.0)

- Status: Accepted
- Date: 2026-07-03

## Context

M4 builds `engine/ecs`, "the world, as data" ([ARCHITECTURE.md](../ARCHITECTURE.md)). Its
"done when" is two proofs: **100k+ entities update in parallel**, and **transforms compose
correctly**. Everything above it — render, physics, destruction — will store its state as
components in this world, so the storage model is one of the engine's **hard-to-retrofit
seams** (ROADMAP "seams before features"): getting it wrong is the kind of mistake that
"kills engines." This ADR settles it *before* any ECS code lands, so bricks M4.1–M4.6 have a
cited decision to build on.

The forces at play come straight from VISION and ARCHITECTURE:

- **Data-oriented by default.** Think in arrays of data and the transforms over them, laid
  out for the cache — not deep object hierarchies (CLAUDE.md; VISION #… DOD).
- **Job-system-centric.** The frame is a graph of jobs across all cores; ECS system updates
  are jobs. The storage must hand the `JobSystem` (`core`'s work-stealing scheduler, with
  `parallel_for(count, chunk, body)` as the front door) a clean parallel grain with no hidden
  ordering deps and no false sharing.
- **The allocators must become load-bearing.** ARCHITECTURE says plainly: "the allocators are
  complete but not yet *load-bearing* — the ECS puts them to work at M4." The storage layer is
  where that happens.
- **Editor and networking are coming.** The M9 editor is a live client that needs to know
  *what changed* to sync inspectors ([ADR-0016](0016-editor-is-a-client-of-the-engine.md));
  M11 replicates deltas. Both want **change detection**, which is cheap to design into storage
  now and invasive to bolt on later.

The one foundational question under all of this: **how are components stored?** Two models
dominate real ECS implementations.

- **Archetype (table / chunk) storage.** Entities that share the *same set of component
  types* (their **signature**) are grouped together; within a group each component type is a
  contiguous array — **structure-of-arrays (SoA)**, one column per type. Iterating a query
  that matches an archetype is a linear scan over packed columns. *Cost:* adding or removing a
  component changes an entity's signature, so the entity **moves** to a different archetype
  (its components are copied across). Used by Unity DOTS, Flecs, and (hybrid) Bevy.
- **Sparse-set storage.** Each component type owns an independent sparse set — a dense value
  array plus a sparse entity→index map. Add/remove a component is O(1) with no move. *Cost:* a
  multi-component query intersects sets and hops between per-component dense arrays that are
  not co-located in memory, so the hottest path is less cache-coherent and a poorer parallel
  grain. Used by EnTT (its default).

Rime's dominant hot path is unambiguous: **run a system over every entity that has {A, B,
C}**, every frame, across all cores. Structural change (add/remove a component) is comparatively
rare and bursty.

## Decision

**Archetype / SoA storage, in fixed-size chunks drawn from `core`'s allocators, with
generational-handle entity IDs and per-component change detection built in from day one.**

Five parts:

1. **Archetype/SoA over sparse sets.** Entities are grouped by signature; each component type
   is a contiguous column. The common query iterates packed columns linearly — cache-optimal
   and trivially chunk-parallel. Add/remove component = **archetype move** (copy the entity's
   components to the archetype for the new signature, swap-remove it from the old). We optimize
   the frequent operation (iteration) and pay for the rare one (structural change).

2. **Chunked columns.** An archetype does not store each column as one ever-growing vector; it
   stores its rows in fixed-size **chunks** (starting figure **16 KiB**, tunable), each holding
   the SoA columns for up to *N* entities where *N* is `chunk_capacity = 16 KiB / row_stride`.
   Chunks are the engine's unit of three things at once:
   - **allocation** — chunks are drawn from `core`'s pool/arena allocators, which is what finally
     makes the allocator module load-bearing (ARCHITECTURE's stated M4 intent). Bounded,
     recyclable allocations instead of unbounded per-column `realloc`s that would invalidate
     addresses mid-frame.
   - **parallel grain** — `parallel_for` hands *whole chunks* to workers, so two threads never
     write the same cache line and a system's parallelism is "one chunk per task." This is the
     M4.4 scheduler's natural unit.
   - **change-detection scope** — version stamps live per chunk (part 4).

   (16 KiB chunks mirror Unity DOTS; the exact size is a profiling knob, not a contract.)

3. **Entity IDs are generational handles.** An `Entity` is `core::Handle`-shaped — a 32-bit
   `index` + 32-bit `generation` (see `handle.hpp`, already documented as "the backbone of
   data-oriented … entity references across the engine (ECS…)"). A purpose-built **entity
   directory** maps the stable `index` → the entity's current `{archetype, chunk, row}` location
   and its live `generation`, with a free list recycling indices and a generation bump on
   despawn so a stale `Entity` is *detected*, not silently aliased to a recycled slot's new
   occupant. The directory is deliberately a **flat, directly-indexed** table rather than a
   `SlotMap<Location>`: entity lookup must be a single direct index with no slot→dense
   indirection, and the dense hot path is *archetype* iteration, not directory iteration — so
   SlotMap's dense-compaction feature would be unused weight here. `SlotMap` (slot_map.hpp)
   remains the sibling pattern and the teaching reference for the generational-recycling idea.

4. **Change detection via per-component version stamps.** A world-global version counter ticks
   monotonically (per frame, and finely per system). Every chunk stores, per component column,
   the version at which that column was last written; a system that writes component C stamps
   C's version on each chunk it mutates. A consumer can then ask "did C change on this chunk
   since version V?" and **skip untouched chunks**. First user is M4.5's transform-hierarchy
   dirty propagation (don't recompute world transforms for chunks whose locals didn't move);
   later users are the M9 editor's live inspector sync and M11's replication deltas. This is
   cheap now (a few bytes per column per chunk + a writer discipline) and invasive to retrofit,
   so it goes in the storage layer immediately even though its consumers land later.

5. **Components are plain, trivially-relocatable data, registered once — through reflection.**
   A component type is registered (M4.1) to obtain a stable `ComponentId` and a small
   type-erased descriptor: **size, alignment, and move/destroy operations**, so a chunk can
   relocate rows during an archetype move without knowing the concrete type. Registration flows
   through the existing **reflection** system (`RIME_REFLECT_*`, extended with type name +
   default construction): a component registered once is **serializable now** (M1.7's machinery)
   and **editor-inspectable later** (M9) — no second registration. Components must be trivially
   relocatable (bitwise-movable) for M4; non-relocatable components are out of scope and revisited
   only when a real need appears.

## Consequences

**Good**
- The dominant hot path — iterate every entity matching a signature — becomes a set of linear
  SoA scans, cache-optimal and chunk-parallel on the job system. This is directly M4's "100k+
  update in parallel" proof and the DOD north star, not a bolt-on.
- Chunks make `core`'s allocators load-bearing (the stated M4 intent), give the M4.4 scheduler a
  clean parallel grain with no false sharing across workers, and host the change-detection stamps
  — one mechanism serving allocation, parallelism, and change tracking.
- Entity IDs inherit the handle's use-after-free detection for free; a dangling `Entity` is
  caught by generation mismatch rather than reading a recycled entity's data.
- Change detection exists in storage from the first commit, so the editor's live sync, incremental
  transform updates, and networked-destruction deltas retrofit **onto a seam that already exists**
  instead of forcing a storage rewrite at M9/M11.
- Registering components through reflection means "described once ⇒ serializable + inspectable,"
  aligning M4 with the editor-enabler direction (ADR-0016).

**Costs we accept**
- **Add/remove component moves an entity** — copying its components to the new archetype and
  swap-removing from the old, O(entity's data) not O(1). Accepted because iteration, not
  structural change, is the hot path; if churn shows up in a profile we can batch structural
  changes through deferred command buffers (applied at a system boundary) without touching the
  storage model.
- **Archetype fragmentation** — many distinct signatures ⇒ many small archetypes ⇒ thinner
  chunks and shorter scans. Acceptable for our workloads; a thing to *measure* (measure before
  optimize), not pre-solve.
- **Trivially-relocatable components only** in M4 — a pragmatic constraint that keeps a chunk
  move a `memcpy`. Revisited if a real component needs a non-trivial move.
- **A per-column version stamp and a writer discipline** — a few bytes per chunk and the rule
  that mutating systems stamp what they touch. Cheap; the payoff (skippable work, live sync,
  deltas) is later.

## Alternatives considered

- **Sparse-set storage (EnTT-style).** O(1) add/remove and simple, and genuinely better when
  structural churn dominates or systems touch one component at a time. But multi-component
  iteration hops across per-component dense arrays that are not co-located, costing locality on
  exactly our hottest path, with a fuzzier parallel grain. Rejected as the *primary* model — but
  the door is left open to a sparse *secondary* index for rare, tag-like or churny components,
  added later without disturbing the archetype core.
- **One growing vector per component per archetype (unchunked).** Simpler than chunks, but no
  natural allocation / parallel / change-detection grain, unbounded reallocations that invalidate
  addresses mid-frame, and false sharing at column boundaries. Chunks are worth the modest extra
  complexity precisely because they unify those three concerns.
- **Entities as `SlotMap<Location>`.** Reuses tested code and gives dense iteration of live
  entities — but adds a slot→dense indirection on every entity lookup for a feature (dense
  directory iteration) we don't use on the hot path. A flat, generation-stamped directory is the
  leaner fit; `SlotMap` stays the documented sibling for the recycling idea.
- **Per-entity component bitmask, no archetypes.** Iterate all entities and test a mask per
  system. Simple, but it is neither SoA nor cache-friendly at 100k+ and defeats the whole reason
  we're here. Rejected.
- **Defer change detection to M9/M11.** Nothing consumes it until M4.5 at the earliest, so
  deferring is tempting — but retrofitting version stamps into live chunk storage is invasive,
  whereas designing them in now is a few bytes and a discipline. Included now.

---

*This ADR is brick **M4.0**. The code lands next: **M4.1** entity directory + reflection-based
component registration; **M4.2** archetype/chunk storage + archetype-move; **M4.3** queries +
chunk-wise iteration; **M4.4** the parallel system scheduler on the `JobSystem`; **M4.5** the
transform hierarchy (change-detection's first consumer); **M4.6** the proof sample
`samples/05-ecs-playground` (100k+ entities, transforms composing). See
[ROADMAP.md](../ROADMAP.md) → M4.*
