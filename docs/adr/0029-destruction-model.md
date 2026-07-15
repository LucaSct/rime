# ADR-0029: The destruction model — cooked fracture patterns, compound instances, contact-driven damage

- Status: Accepted
- Date: 2026-07-15

## Context

M8 is Destruction v1 — the first pillar of the Frostbite-class vision (VISION #1), and the ROADMAP's
"done when": *a wall fractures on impact, debris falls/settles, one event drives a VFX+sound stub.*
Destruction is not a module bolted onto physics; the M7 physics core was **built as its substrate**,
brick by brick, and this ADR is where those seams become a destruction model. What already shipped:

- **Compound shapes** ([ADR-0028](0028-compound-shapes.md)) — "one body, many convex children" was
  designed *as* the intact destructible: an intact wall is ONE rigid body whose collision shape is
  its fracture cells.
- **Convex hulls** ([ADR-0027](0027-convex-hull-shapes.md)) — a world-owned store, `register_hull →
  HullId`; detached debris are hull bodies, registered once per pattern (the shape economy).
- **Per-region contact events** (M7.9/M7.12, `events.hpp`) — `ContactEvent.normal_impulse` is the
  solver's summed ∫F·dt for a contact region, and `child_a`/`child_b` name the struck child. This is
  the damage signal, already part-addressed.
- **CCD** (M7.10) — a fast projectile is arrested at a thin wall and its stop **emits a contact
  event carrying the hit impulse**: the projectile-damage path, for free.
- **Scene queries** (M7.7) — `overlap_sphere` + `apply_impulse` are the explosion's "what is in the
  blast radius, push it" primitives.
- **WorldStats** (M7.13) — deterministic per-tick body/collision/island counts: the debris budget's
  instrument. **Sleep events** (`Slept`) are the "debris settled" signal.
- **Determinism** ([ADR-0026](0026-physics-core.md), `world_hash`) — bit-identical across runs and
  worker counts. M11 networked destruction *replays events against this*, so the entire damage →
  fracture path must be a deterministic function of its inputs.

The pre-M7 plan skeleton (`~/projects/rime-plans/m8/`, 2026-07-06) could not see any of this; its
invariants survive but several "decide by measuring" questions are now settled by shipped design,
and two obligations it never named (store lifetime; the compound-immutability consequence) are now
forced. This ADR re-cuts the model and pins what m8.1–m8.6 cite. It decides the *model*, not the
algorithms (the fracture cook's geometry is [ADR-0024](0024-asset-model.md)'s asset shape plus
`docs/design/destruction.md`; the connectivity math is its own note).

## Decision

A **destructible** is a cooked *fracture pattern* — a set of convex **parts**, a **bond** graph
recording which parts are glued and how strongly, and **anchors** (parts pinned to the world) —
instanced at runtime as **one static compound body**. Damage arrives as accumulated contact impulse
(and explicit API ops); when a part's health reaches zero it dies; a connectivity solve finds parts
no longer supported by an anchor; and **fracture is a body swap** that turns the unsupported set into
free physics bodies. Eight decisions make that concrete.

### 1. Representation: intact = one static compound body

Each intact instance is one `MotionType::Static` body whose shape is the pattern's registered
compound (ADR-0028). The pattern's geometry — every part's hull, and the full compound — is
registered *once* and shared by all instances by id. This was ADR-0028's design intent; it is
recorded here, not re-litigated.

### 2. The fracture transition is a body swap (because compounds are immutable)

A registered compound is immutable (ADR-0028), so a damaged wall **cannot lose a child in place** —
the central mechanic of M8 is therefore a controlled replacement, run in the sequential tail:

- destroy the instance's body;
- `register_compound` the **anchored remainder** (children = the surviving anchored parts, in
  ascending part id — a deterministic id) and create a fresh static body for it;
- each detached connected component becomes a new **dynamic** body: a single part → a hull body; a
  multi-part island → a runtime-registered **dynamic compound** (decision ratified: multi-part
  islands keep their shape — the Frostbite look — not shatter to loose parts);
- the part that just died detaches as a hull body carrying the killing impulse (cheap, and it reads
  right).

Detached debris can take further damage in v1 only as single-part splits; **recursive re-fracture**
of a detached compound is v2 (backlogged). The load-bearing consequence, named loudly: a fracture
event performs *runtime* `register_compound`/`register_hull` calls, so **the shape stores must be
able to release ids** — m8.5 owns `unregister` (ADR-0027/0028 deferred it here). *Rejected as the v1
default — per-part static bodies after first damage* (no re-registration, a direct body→part map,
but M proxies per intact wall — the opposite of ADR-0028's one-proxy economy, and it multiplies the
static broadphase population by the part count of every damaged wall on screen). It stays the
recorded fallback; the revisit trigger is m8.3 measuring runtime `register_compound` cost against it.

### 3. Damage: per-part health, from contact impulse and explicit ops

Each part carries health. Two damage sources, combined into one per-tick op list:

- **contact-derived** — drain `contact_events()` after the step; a region's summed impulse above a
  **cooked threshold** (scaled by a per-part damage material) is damage to the named part. The
  threshold is what fences the resting-support case: a wall's own parts rest on each other exchanging
  `m·g·dt` every tick (a `Persisted` event), and that must *not* erode them.
- **explicit** — an `apply_damage(point, radius, amount, impulse)` API for explosions and scripted
  hits; radius→parts resolves destruction-side against cooked part AABBs (no physics change), and the
  push rides `apply_impulse`.

Determinism (decision 4's sibling): the per-tick op list is applied in a **canonical order** —
explicit ops sorted by (instance, part, op bytes), then contact ops in the already-canonical event
order — so input-arrival order cannot change the outcome. Health crossings *are* the health
transitions the fan-out reports.

### 4. Part identity and the child-index remap

Part ids are cook order, byte-stable forever (ADR-0024 cooks are reproducible). Because the compound
is re-registered on each fracture (decision 2), a part's *child index within its current compound*
changes over its life; the instance keeps a `child index → part id` table (its rows are the surviving
part ids in ascending order, so it is derivable, not guessed). This mapping is the **M11.4 addressing
contract**: an event that says "part 37 died" means the same part on every client.

### 5. Parts are not *simulated* entities

An intact part has no `RigidBody`/`Collider` and runs in no gameplay system — it is a child of one
compound body. The M8-scope refinement of the skeleton's "parts are not entities": parts *may* be
**render-leaf entities** (a `MeshRef` + a static `WorldTransform` under the instance root), because a
static entity costs nothing after spawn under change detection (M7.6) and M4 proved 200k entities.
Decision ratified: **per-part render-leaf entities in v1** — the simplest path to per-part draws;
holding the SoA line and building a render push-API is the measured fallback if m8.2 shows draw
pressure on lavapipe.

### 6. Debris bypasses `PhysicsSync`

The ECS `Collider` cannot name a `HullId`/`CompoundId` (ADR-0027/0028 deferred asset-level shape
identity — which *this cook creates*, but taking it now would couple the physics seam to the asset
system prematurely). So `engine/destruction` creates and destroys debris bodies **directly on
`PhysicsWorld`** and runs its own awake-only write-back over its roster — the `PhysicsSync` pattern,
applied to bodies it owns. The graduation trigger (ECS-authored destructibles once the cook's shape
identity is a first-class asset ref) is recorded, not taken.

### 7. Event transport: a generic `EventChannel<T>`

The health-transition stream (PartDamaged / PartDied / IslandDetached / DebrisSettled) is a
double-buffered per-tick channel in canonical order — the M7.9 discipline (*data, not callbacks*;
fill a back buffer, swap at the tick boundary, expose a span) generalized one layer up as a small
`EventChannel<T>` (decision ratified over a destruction-private buffer, because Track FX's spawn
tables, the M10-C2 lighting listener, and audio all queue behind the same shape). **Physics keeps its
own event spans** — migrating `PhysicsWorld` onto an ECS/engine event type would point the seam the
wrong way. Event payloads carry the **world-space AABB** of the affected part/island (cook-known
bounds × transform) — the M10-C2 "what region changed" ask.

### 8. Budgets are policy; the tick order

Debris caps and eviction are policy that *reads* `WorldStats` (M7.13) — no new physics counters.
Eviction priority is cooked base × size × age, **settled-first**; camera distance is deliberately
**out** of v1 scoring (it is a determinism hazard, and M11 per-client relevancy owns proximity
anyway). Destruction runs in the **sequential tail, after `PhysicsSync::write_back`** — it reads
solved, settled state and mutates the body population between ticks, never during the parallel solve
(`docs/design/simulation-tick.md` gains steps 7–8).

### Two physics seams M8 owns (decision ratified)

Small, additive changes to the physics core, each its own PR with its own proof: **`RayHit::child`**
(hitscan must name the struck part; the compound raycast already finds the nearest child — one
`std::uint16_t` field, at m8.3) and **`unregister_hull`/`unregister_compound`** (the stores grow a
generational free list; ids stay a pure function of the call sequence, so determinism holds — at
m8.5, forced by decision 2). **Static triangle mesh is explicitly not needed for M8** (ground is a
static box, walls are compounds); it stays a fast-follow, triggered by the first real level geometry.

### Placement (the Rust/C++ split)

The fracture **cook is offline tooling → Rust** (`tools/asset-pipeline`, on the ADR-0024 asset model
and cook cache; quickhull, ADR-0027's deferral, lands here as the cook's robustness backstop). The
runtime is **C++** — a new, removable `engine/destruction` module (guardrail 2) depending on `ecs`,
`physics`, `assets`, and render *interfaces*. The RHI seam is untouched: destruction is CPU + a feed
into the render extraction.

## Consequences

- **M8 has a spine:** cook a pattern → register it once → stand instances as static compounds →
  accumulate contact impulse into per-part health → connectivity solve → swap bodies on fracture →
  fan out one event stream. Each step names a shipped M7 seam; the damage-to-part mapping is direct
  because events already name the child.
- **Determinism is preserved end to end** — every ordering choice (op sort, ascending-part-id
  remainders, canonical event order, free-list ids as a pure function of calls) exists so the damage
  → fracture path is the deterministic function M11 replays. This is a *requirement on every M8
  brick*, tested with the `world_hash` + part-state-bitfield discipline the physics suite established.
- **Store lifetime becomes real** (decision 2): the previously-deferred hull/compound `unregister`
  is now a shipped m8.5 obligation, forced by the fracture soak, not a "someday."
- **A body pair's plurality was already paid for** by ADR-0028 (per-region cache/events); M8 consumes
  it rather than extending it. Non-destructible worlds are untouched.
- **Deferred, each with a home:** runtime *recursive* re-fracture of detached compounds → v2, when a
  detached chunk must itself shatter; **per-part materials** → the M8 material system (v1 shares one
  damage material per pattern); the **interior-face aesthetic** (darker exposed faces need a second
  material/submesh) → a later polish brick (v1 draws parts single-material — 50 honest parts beat a
  fragile 100); **static triangle mesh + midphase** → the first real level-geometry collision;
  **ECS-authored destructibles** → asset-level shape identity from the cook; **camera-distance
  eviction** → M11 relevancy. The real VFX/audio are Track FX / `engine/audio` (M8 ships the fan-out
  seam plus a dust stub and a null-audio backend — the stub is built to be *deleted* by fx1).
