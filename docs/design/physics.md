# Physics core — design note (M7)

Companion to `engine/physics` (M7, ADR-0026). This note is the running design record of Rime's
own rigid-body core; it grows a section per brick, in the order the bricks land. The math
derivations live separately in [`docs/math/rigid-body-dynamics.md`](../math/rigid-body-dynamics.md);
this note covers the *systems* reasoning — data layout, algorithms, and the trade-offs behind them.

Current coverage: the world seam and body pool (M7.1), the broadphase (M7.2), the narrowphase
(M7.3), the impulse solver (M7.4). Islands and the parallel step (M7.5) will extend it next.

## Why an own core

ADR-0026 carries the full argument; the one-paragraph version: Rime's physics is written from
scratch — no Jolt, no PhysX — because the engine is a teaching codebase (a wrapped solver teaches
nothing), because Frostbite-class destruction (M8) wants collision structures shaped for parts
that split and re-pool at runtime, and because networked destruction needs *same-binary
determinism* — identical inputs must produce bit-identical worlds, across runs and (from M7.5)
across thread counts. Owning every algorithm is what makes all three tractable.

## The world and the body pool (M7.1)

`PhysicsWorld` is the module's entire public seam. Broadphase, narrowphase, solver, and storage
all live behind a pImpl in `src/` — the same discipline as the RHI: nothing above the seam ever
sees an internal header, so every one of those internals can be rewritten without touching a
caller.

Bodies live in a data-oriented **SoA pool**: parallel dense arrays (`position[]`,
`orientation[]`, `linear_velocity[]`, …) indexed `[0, count)`, kept dense by swap-remove, with a
generational slot table mapping a stable `BodyId` to the current dense index (the
[slot-map pattern](slot-map.md)). Hot loops — integration, and now the broadphase update — walk
contiguous memory; a stale `BodyId` reads as dead instead of aliasing a reused slot.

Integration is **semi-implicit (symplectic) Euler** — velocity first, then position with the new
velocity — the standard game integrator because it is energy-stable for oscillatory systems where
explicit Euler visibly pumps energy. The derivation, the quaternion update `q̇ = ½·ω⊗q`, and the
primitive inertia tensors are in the math note.

The module is GPU-free by construction: collision geometry enters as plain data, never GPU
resources, so every physics proof runs on lavapipe CI.

## Broadphase (M7.2)

### The problem: the pair explosion

Collision detection naively means testing every body against every other: `n(n−1)/2` pairs. At
destruction scale — say 5,000 debris pieces — that is ~12.5 **million** exact shape tests per
tick, and exact tests (GJK, M7.3) are the expensive kind. But almost every one of those pairs is
trivially far apart. The broadphase exploits that: wrap each body in the cheapest useful bound —
an axis-aligned bounding box, six floats, overlap testable in six comparisons — and report only
the pairs whose *boxes* overlap. The output is a small **superset** of the truly touching pairs
(a fat box may overlap where the shapes don't); the narrowphase then confirms or rejects each
candidate exactly. Conservative in exactly one direction: the broadphase may over-report, but it
must never miss a pair the exact test would find.

### Fat AABBs: paying a little space to skip most of the work

The tree does not store a body's *tight* AABB but a **fat** one — the tight box expanded by a
margin (`kMargin`, 10 cm) on every side. The payoff is in the per-step update: after integration,
`move_proxy` checks whether the body's new tight box still fits inside the stored fat box, and if
so does *nothing*. A body moving a few millimetres per tick — almost everything, almost always —
stays inside its margin for dozens of frames, so the tree is barely touched per step. Only a body
that outruns its margin is removed and re-inserted with a fresh fat box around its new position.

The cost is mild over-reporting: fat boxes overlap slightly before tight ones do, so a few extra
candidates flow to the narrowphase, which rejects them. That is the designed trade — narrowphase
rejections are cheap and parallel; structural tree edits are the thing worth avoiding.

One consequence worth stating precisely, because the tests lean on it: the broadphase's answers
are defined over the **stored** fat boxes, which can be *staler* (fatter) than `tight + margin`
recomputed this instant — a coasting body keeps the fat box from wherever it was last
re-inserted. That is still correct: the stored box contains the tight box at all times (that is
the re-insert condition), so stored-box overlaps remain a superset of tight overlaps. The
brute-force oracle in `tests/physics/broadphase_test.cpp` therefore compares against the stored
boxes (`broadphase_aabb`), not freshly recomputed ones.

### The dynamic AABB tree

The acceleration structure (`src/aabb_tree.hpp`, private) is a binary **bounding-volume
hierarchy** in the Box2D/Bullet lineage: leaves hold the fat boxes, every internal node bounds
its two children, the root bounds the world. A query descends only the branches whose bounds
overlap the query box — on a reasonably shaped tree that is O(log n) instead of O(n), and it is
read-only, so parallel query jobs (M7.5/M7.7) can share the tree with no locking.

Insertion is guided by the **surface-area heuristic (SAH)**: the probability that a random query
box intersects a node scales with the node's surface area, so expected query cost is the sum of
node areas — and the best home for a new leaf is the one that grows that sum least. The insert
walks down from the root, at each node comparing the cost of pairing up right here against the
cost of descending into either child (the box growth it would cause, plus the growth inherited by
everything above), and stops when descending can't win.

Two data-layout choices repeat engine-wide patterns: nodes live in a pooled array linked by
*indices* (stable when the pool's vector grows — the same reasoning as generational handles), and
freed nodes chain onto an intrusive free list for O(1) reuse, so churn does not fragment.

Deliberately **not** here yet: Box2D follows every structural change with AVL-style tree
rotations to bound the height under adversarial insertion orders. Rotations change only the
tree's *shape*, never its *answers* — every leaf stays bounded by all its ancestors either way —
so the tree is exact without them; only worst-case descent depth suffers. That is a performance
concern, and performance work in this module is measured, not guessed: rebalancing lands at
M7.10 against the debris-scale stress harness, if the numbers ask for it.

### Dual trees: the static world is not dynamic

`PhysicsWorld` keeps **two** trees: a `static_tree` for bodies that never move and a
`dynamic_tree` for everything that can (dynamic and kinematic). In a Battlefield-scale scene the
overwhelming majority of collidable geometry is static — terrain, intact buildings, props — and
folding it into the tree that churns would be pure waste. The split buys three things:

- The static tree is built once and never refit — no fat-margin bookkeeping, no re-inserts, a
  shape that only improves as the SAH sees the whole world up front.
- The dynamic tree — the one that takes structural edits every time something outruns its
  margin — stays small: movers only.
- **Both-static pairs are impossible by construction.** Pair collection works by having every
  *mover* query both trees; static bodies never query. Two static bodies can therefore never
  report each other — there is no filter to forget, the code path simply does not exist. (A
  static–static contact would be meaningless anyway: neither body can respond.)

When destruction (M8) converts a wall from intact to rubble, the wall's static proxy is destroyed
and its parts enter the dynamic tree — the seam is shaped for that hand-off.

### The canonical pair list

`compute_pairs` turns tree queries into the thing the narrowphase actually consumes: each mover
queries both trees with its fat box; every hit `(self, other)` is packed into a single `uint64`
key, `(min_slot << 32) | max_slot`; the key vector is sorted and de-duplicated. The packing makes
three properties fall out of one sort:

- **Canonical pairs** — the smaller slot id always comes first, so `(a,b)` and `(b,a)` are the
  same key. A dynamic–dynamic overlap is discovered twice (each end queries) and collapses to one
  entry in the `unique` pass.
- **Canonical order** — the list is strictly ascending by `(a, b)`, so downstream consumers
  (contact caching, events, replay logs) see a stable ordering with no effort of their own.
- **Determinism** — the output is a pure function of the body state, independent of tree shape
  history or discovery order. This is the property M7.5 preserves when the per-mover queries move
  onto the job system: queries can complete in any order on any thread count, and the sort still
  produces the identical list. Same-binary replays (M11) and networked destruction stand on this.

### Who consumes it next

M7.3's narrowphase runs GJK/EPA over exactly this candidate list. M7.7's ray/overlap/shape-cast
queries reuse the same two trees (a ray descends the BVH the same way a box does). M7.9's
triangle-mesh midphase reuses the *structure* — an AABB tree over triangles instead of bodies.

### Proofs

`tests/physics/broadphase_test.cpp`, all pure CPU: equivalence with a brute-force O(n²) oracle
over the stored fat boxes (before and after stepping, so the stale-fat-box contract is really
exercised); canonical order + run-to-run determinism of the pair list; both-static exclusion;
the fat-margin no-op fast path (bit-identical stored bounds after a sub-margin move); analytic
`compute_aabb` checks per shape; and structural validity under seeded create/destroy churn —
the pooled, index-linked tree's UB net, run under ASan/UBSan at the brick boundary.

## Narrowphase (M7.3)

### From candidate to contact

The broadphase hands over a small superset of possibly-touching pairs; the narrowphase answers the
exact question each one poses — *do these two shapes really overlap, and if so along what normal,
how deeply, and where?* — and packages the answer as a **contact manifold** (`contact.hpp`): a
shared unit normal plus up to four contact points, each carrying a penetration depth, a surface
position, and a stable **feature id**. That manifold is the entire input to the M7.4 solver;
everything needed to push the pair apart and apply friction lives in it.

One convention is fixed here because everything downstream depends on it: the normal is unit length
and points **from body `a` toward body `b`** — the direction to push `b` to separate them — with
`a`/`b` in the broadphase's canonical slot order, so the sign is reproducible run to run.
Penetration is non-negative depth along that normal; a point sits midway between the two surfaces
(the two coincide as depth → 0). `compute_contacts` walks the broadphase's canonical pair list,
narrowphases each surviving pair, and emits manifolds in that same order.

### Two routes: fast paths and the general convex pipeline

Not every pair earns the same machinery. The dispatcher (`collide_shapes`) canonicalizes a pair by
shape type and routes it:

- **Analytic fast paths** for any pair with a sphere or a capsule. A sphere is a point inflated by
  a radius; a capsule is a *segment* inflated by a radius. The closest-point problems for points
  and segments have exact closed forms (point clamped into a box, segment-to-segment), so these
  pairs never iterate: collide the shrunk cores, then add the radii back — the *shrunk-shape trick*.
  A closed form is cheaper and more robust than iteration, and a sphere resting on a floor should
  never hinge on a convergence tolerance.
- **The general convex pipeline** — GJK → EPA → clipping — for box–box, and at M7.9 anything with a
  convex hull. It speaks to geometry *only* through a support function, which is exactly why the
  same code will run arbitrary hulls unchanged.

The derivations — the Minkowski-difference reformulation, the simplex distance subalgorithm, the
polytope expansion — live in [`docs/math/gjk-epa.md`](../math/gjk-epa.md); this note covers how the
pieces assemble into a manifold.

### GJK and EPA: overlap, then depth

**GJK** decides overlap by a reformulation: two convex sets `A` and `B` intersect exactly when
their Minkowski difference `A − B = { a − b }` contains the origin. GJK never builds that set — it
samples it through the support function `support_{A−B}(d) = support_A(d) − support_B(−d)` and walks
a *simplex* (1–4 sampled points) toward the origin. Separated → it reports the distance and the
closest points on each shape (the *witnesses* the fast paths and, at M7.10, speculative contacts
reuse); overlapping → its terminal simplex seeds EPA.

**EPA** then answers "by how much." With the origin inside the difference, the penetration vector
is the shortest translation carrying the origin to the difference's boundary. EPA grows a polytope
inside the difference from GJK's simplex, repeatedly pushing out its origin-nearest face until the
support function says that face lies *on* the boundary: its normal is the contact normal, its
distance the depth. The robustness posture is stated plainly in the code and the math note —
float-only, metre-scale tolerances, degenerate simplices inflated by probing fixed axes, and on any
numeric dead end EPA returns its best face so far rather than looping. A pair whose difference is
numerically flat is dropped for the tick (a documented, conservative miss).

### From one direction to a patch: reference-face clipping

EPA yields a single deepest direction and one witness point — enough to un-overlap two spheres, but
a box resting on a box that way would balance on one point and seesaw. A resting face needs a
*patch*. So for box–box the pipeline clips: choose the **reference face** (the face most aligned
with the contact normal; ties break toward `a` so the choice — and every feature id below it —
cannot flip frame to frame on floating-point noise), clip the other box's most anti-parallel
(**incident**) face against the reference face's four side planes (Sutherland–Hodgman), keep the
points that lie below the reference plane, and if more than four survive, **reduce** to four by a
deterministic greedy pick — the deepest point, then the farthest, then the two adding the most
spread — so the solver anchors the largest stable patch. The manifold normal is snapped to the
reference face rather than kept as EPA's raw polytope-triangle normal, which jitters at float
precision; the two agree whenever the contact really is face-driven.

### Feature ids and warm starting

Every contact point is tagged with a **feature id**: a hash of the shape features that generated it
— a box corner or face, a capsule end, or an (incident edge × reference side-plane) clip cut. The
id is a pure function of contact *topology*, so the same physical contact reproduces the same id
every frame while distinct points in one manifold get distinct ids. That stability is the whole
point: a **persistent manifold cache** — keyed by the canonical body pair, guarded by the bodies'
generations — matches this frame's points to last frame's *by feature id* and carries their
accumulated impulses forward. That is **warm starting**, and it is the difference between a stack
that stands and one that buzzes: the M7.4 solver begins each tick from last tick's near-converged
impulses instead of from zero. M7.3 builds and exercises the cache but stores zeros (there is no
solver yet); M7.4 fills the impulses in. The generation guard means a recycled body slot can never
inherit a dead pair's impulses — the correctness the churn test pins down.

### Determinism

Every routine is a pure function of its inputs, deliberately: support and face selection break ties
by a fixed scan order, clip output keeps a fixed winding, the manifold reduction breaks ties by
lowest index, and `compute_contacts` visits pairs in the broadphase's canonical order. Same binary,
same inputs → identical manifolds, feature ids included — the property M7.5 preserves when the
narrowphase moves onto the job system, and M11 leans on for replay.

### Proofs

`tests/physics/narrowphase_test.cpp`, pure CPU: analytic sphere–sphere, sphere–box, and
sphere–capsule depth and normal; a box-on-box **four-point** manifold at the overlap-rectangle
corners; EPA minimum-translation-axis selection under a deep lateral overlap; feature-id
frame-stability observed through the warm-start cache (and the cache clearing when a contact
separates); run-to-run determinism through stepping; and seeded create/destroy churn that also pins
the cache's generation guard — the pointer-heavy GJK/EPA path's UB net, run under ASan/UBSan at the
brick boundary.

## Solver (M7.4)

### From manifolds to motion

The narrowphase's manifolds describe *where* bodies touch; the solver decides what that touch
*does*. From M7.4 a `PhysicsWorld::step` is the full sequential-impulse pipeline: integrate
velocities (gravity + damping) → detect contacts (broadphase → narrowphase, warm-started from
the persistent cache) → prepare constraints and re-apply last tick's impulses → a fixed count of
velocity iterations → integrate positions with the *solved* velocities → a fixed count of NGS
position iterations → commit the solved impulses back to the cache. The placement of the
position integration is the semi-implicit pairing doing its job at the pipeline scale: gravity
enters the velocity, the solver cancels it against the contacts in the same tick, and only then
do positions move — a resting body never accumulates downward motion it must later undo.

The mathematics — the contact constraint, effective mass, accumulate-and-clamp, the friction
pyramid, restitution, and the NGS derivation — lives in
[`docs/math/sequential-impulse.md`](../math/sequential-impulse.md); this section covers the
system choices around it.

### Sequential impulses over the manifolds

The velocity solve is projected Gauss–Seidel in impulse space (`src/solver.hpp`): sweep every
contact point in the manifolds' canonical broadphase order, solve each point's one-dimensional
problem against the bodies' *current* velocities, clamp its accumulated impulse (normals never
pull; friction lives in the pyramid |λ_t| ≤ μ·λ_n), apply the change immediately. Applying
immediately is what propagates support up a stack within a single sweep. Iteration counts are
fixed — **8 velocity / 2 position** (ADR-0026) — never convergence-tested, because an early-out
would make the float-op sequence data-dependent and break bit-identical replays.
Under-convergence with a fixed budget shows up as mild softness, and warm starting is what makes
the budget sufficient.

Materials ride the body pool as two more SoA columns (`friction`, `restitution`, from
`BodyDesc`), combined per pair as μ = √(μ_a·μ_b) and e = max(e_a, e_b). Restitution applies only
above a 1 m/s approach (a resting body re-approaches at g·dt every tick — reflecting that is
permanent jitter), and the friction tangent basis is derived from the normal alone (least-aligned
world axis), so a persistent contact re-derives the same basis every tick and the cached tangent
impulse keeps its meaning.

### Warm starting closes the loop

M7.3 built the persistent manifold cache and matched points across ticks by feature id, but
could only ever carry zeros. M7.4 closes the loop — and the closing required an ordering fix
worth recording: the cache must be committed **after** the solve, from the solved manifolds, not
at detection time. A cache committed at build time would forever re-persist the warm-start
values it had just read (the solver's converged impulses would never enter it), and every tick
would effectively start cold. `step()` therefore builds warm-started manifolds, solves (the
iterations mutate each point's accumulated impulses), and only then commits. The public
`compute_contacts` inspection seam keeps its M7.3 semantics — build then commit with no solve in
between — which changes nothing observable (it re-commits the values it read) and keeps the
narrowphase proofs valid unchanged.

At rest the loop is exact: the warm start applies last tick's impulses, the pre-loaded contact
already cancels this tick's gravity, and the iterations find nothing left to correct — the
converged accumulators equal the applied totals, tick after tick. The proof measures exactly
this: a resting box's persisted normal impulses sum to m·g·dt.

### The NGS position pass — not Baumgarte

Velocity-level contact keeps bodies from approaching further but cannot remove overlap that
already exists (discrete detection notices a fast body up to one tick late). The rejected cure,
Baumgarte, folds β/dt·penetration into the velocity bias — one solver, but the recovery velocity
is *real* kinetic energy: debris pops, stacks hum, nothing ever sleeps. Rime runs a separate
non-linear Gauss–Seidel pass over **poses only** after integration: per point, a pseudo-impulse
in displacement units (same effective-mass machinery) moves positions and nudges orientations,
re-measuring the true separation from body-local anchors each iteration. The velocity arrays are
never touched — no energy can enter, by construction. Tuning: 5 mm slop is deliberately left in
place so a resting manifold (and its warm-start cache) survives tick to tick; 20% correction per
iteration converges without Gauss–Seidel ping-pong; a 20 cm per-point cap bounds the teleport a
spawn-inside-a-wall can cause in one tick.

### Determinism and island-readiness

Every stage runs in dense-index or canonical-pair order with fixed iteration counts; the contact
cache is consulted by keyed lookup only (nothing ever iterates the hash map in the sim path); the
tangent basis is a pure function of the normal; no fast-math (CI keeps it out of the build).
Same binary + same inputs ⇒ bit-identical world state, asserted through full contact scenes.
Deliberately island-ready: every solver routine is a pure function of {one constraint, the two
bodies' pool rows} with no cross-constraint state, so M7.5 can partition the constraint list by
island (islands share no dynamic body) and run the identical loops per island — bit-identical
for any thread count, which is the milestone's determinism thesis.

### Proofs

`tests/physics/solver_test.cpp`, all pure CPU and analytic: a dropped box rests at the closed-form
height (ground + half-extent − slop) with bounded penetration and ~zero velocity; restitution
e = 0 never rises above rest while e = 1 rebounds to ~the drop height (energy window, first
apex); a box on an incline holds below the friction angle atan(μ) and slides clearly above it
(the tangent basis puts t1 exactly along the slope, so the pyramid bound is exact there); a
three-box stack stands without creep or sinking; the persisted normal impulses of a resting
manifold sum to m·g·dt and `contacts_warm_started_last()` proves the cache carries them; a
head-on equal-mass impact conserves momentum to float precision with e = 1 reflecting and e = 0
stopping dead; the NGS discriminator — two boxes spawned deeply overlapped at rest separate with
speeds staying *numerically zero* (the test Baumgarte cannot pass); and the full pipeline is
bit-identical run to run over a colliding, rubbing, bouncing scene. The suite runs under
ASan/UBSan at the brick boundary (the new SoA columns and constraint buffers are swap-remove and
lifetime surfaces).

## Islands, sleeping & the parallel step (M7.5)

The solver from M7.4 already scales *within* a contact scene. M7.5 makes it scale *across* the
machine and stop paying for scenes that have gone still — without giving up a single bit of
determinism. Three ideas, one data structure.

### Islands: the connected components of the contact graph

Picture the tick's contacts as a graph: dynamic bodies are nodes, and each contact between two
dynamic bodies is an edge. Its connected components are the **simulation islands** — maximal sets
of bodies that can influence each other this tick. A ball resting on a crate that rests on a second
crate is one island; a crate on the far side of the level is another. Two islands share no dynamic
body *by construction*, which is the property everything else here leans on.

The one modelling subtlety is the world itself. Static and kinematic bodies are **not** graph
nodes: a contact against the ground does not union the two crates resting on it. If it did, every
object touching the floor would collapse into one giant island that could never be split across
cores or slept piece by piece — the floor would glue the whole level together. So only a
dynamic–dynamic contact merges components; a contact against an immovable anchor just attaches to
its dynamic end.

`src/islands.hpp` computes this with a **union-find** (disjoint-set) forest over the dynamic bodies:
walk the constraints, union the dynamic–dynamic pairs, then read off the components. Two details buy
determinism for free. Union *by smaller index* keeps each set rooted at its lowest dense body, and a
first-seen scan in dense order assigns island numbers — so island 0 always holds the lowest-indexed
body and the whole numbering is a pure function of the partition, never of iteration luck. The
result is emitted as a **CSR** (compressed-sparse-row) pair of arrays — bodies grouped by island,
constraints grouped by island — with members dense-sorted and constraints in their canonical
broadphase order. The buffers are reused across ticks, so a warmed-up world does no per-step island
allocation (the full allocator story is M7.6; this just avoids the obvious churn).

### The parallel step: same math, more threads, identical answer

Because islands share no dynamic body, an island's solve reads and writes only its own bodies. The
M7.4 solver was written for exactly this — every routine is a pure function of one constraint and
its two body rows — with one gap closed here: `apply_impulse` and the NGS pose update now *skip* the
write to any immovable body instead of storing an unchanged `−= 0`. That is numerically a no-op, but
it makes static and kinematic anchors strictly **read-only** during the solve, so two islands that
rest on the same floor never touch the same memory. With that, `step()` hands the islands to the
`core::JobSystem` — each worker solves whole islands, over-decomposed into a few chunks per thread so
the work-stealing scheduler balances the (wildly uneven) island sizes.

The payoff is the milestone's thesis: **the result is bit-identical no matter how many threads run
it.** A single island's per-body sequence of floating-point operations is the same whether it runs
alone or beside forty others, because nothing else writes its rows; and *which* thread runs it, or
how the islands are chunked, changes only the timing. Running the islands one after another
(`set_job_system(nullptr)`) and running them across four workers produce the same `world_hash()` to
the bit. `world_hash()` — an FNV-1a fingerprint of every body's full motion state — is the witness,
and it is the same hook networked-destruction determinism and replay validation will reuse.

### Sleeping: stop integrating what has stopped moving

A settled stack should cost nothing. A body is a **sleep candidate** while both its linear and
angular speed stay under a small threshold; once *every* body in an island has been a candidate for
half a second, the whole island **sleeps** — its velocities are zeroed and, from the next tick, it
is skipped by integration and the solver entirely. Islands sleep as a unit because a body cannot
safely rest while a neighbour it is stacked on is still settling.

Two subtleties. First, sleepiness is judged on the **post-solve** velocity, not the post-gravity
one: a resting body re-enters every tick carrying the `g·dt` gravity just added, which the contact
solver then cancels — read it before the solve and nothing would ever sleep. Second, **waking is
free**. A body that falls onto a sleeping stack is its own awake island until it lands; on contact,
union-find merges it into the stack's island, and an island with any awake member is reactivated
whole. Broad- and narrowphase keep running for asleep bodies precisely so that first contact is
seen. `wake_body()` and disabling sleeping are the explicit escape hatches for game code that
teleports or shoves a body from outside the simulation.

Sleeping is decided sequentially, between the parallel regions, so it can never perturb the
cross-thread hash — the determinism proof is run with sleeping both off (every island busy every
tick, the hard case) and on.

### Proofs

`tests/physics/islands_test.cpp`, pure CPU and structural: two crates far apart on a shared floor
are two islands while two stacked crates are one (the static-anchor rule, checked through
`islands_last()`); a resting box sleeps well inside its budget and then holds a bit-stable
`world_hash()` tick after tick (the "costs nothing" property); a box dropped on a sleeper wakes it
on impact and the pair later re-sleeps; `wake_body()` and `set_sleeping_enabled(false)` both
reactivate a sleeper. The headline is determinism: a multi-island scene (three separated stacks plus
two fallers) stepped 300 times yields one `world_hash()` shared by the sequential path and by 1-, 2-,
3-, and 4-worker job systems — proven with sleeping off *and* on. The whole suite is green under TSan
(the parallel solve is the threading surface it exists to net) and ASan/UBSan at the brick boundary.

## Change detection & ECS sync (M7.6)

The physics core simulates a pool of `BodyId`s; the game thinks in *entities* with components. M7.6
is the seam between them, and the point where two earlier investments — the fixed tick (ADR-0023)
and sleeping (M7.5) — cash out against a third, ECS change detection (ADR-0018 §4). The full
per-tick order lives in [simulation-tick.md](simulation-tick.md); this section is the physics-side
summary.

### The bridge: bind, write-back, unbind

`PhysicsSync` (`physics/sync.hpp`) holds the one piece of state neither side owns alone — the
entity↔body **roster** — and does three jobs across a tick:

- **Bind.** An entity carrying the intent (`RigidBody` + `Collider` + `WorldTransform`) but no body
  yet gets one created *at its transform*, plus a `RigidBodyHandle` component linking the two. The
  intent components stay pure authored data (reflected, serialized); the handle is transient runtime
  bookkeeping (unreflected — a fresh bind regenerates it). This keeps "what the entity *wants*"
  (what the M9 inspector shows) orthogonal to "which body id backs it right now."
- **Write-back.** After `step()`, each **awake** dynamic body's position and orientation are written
  into its entity's `WorldTransform`, and that component is stamped changed. Sleeping, static, and
  kinematic bodies are skipped — they did not move, so they neither write nor stamp.
- **Unbind.** A body whose entity was despawned (or dropped its `RigidBody`/`Collider`) is
  destroyed. A despawn takes the `RigidBodyHandle` with it, so no query could rediscover the
  orphaned body — which is exactly why the roster exists as the authoritative cleanup list. Bind and
  unbind are structural (they add/remove components), so `reconcile()` runs between phases on the
  main thread, never inside a query over those archetypes.

Scale is left alone: a simulated body's `WorldTransform` has its translation and rotation written by
physics, its scale untouched (physics ignores scale in v1 — a body's extents come from its
`Collider`). Kinematic push-in (game → body) and physics → `LocalTransform` for parented bodies are
deferred seams; v1 treats a simulated body's world pose as physics-owned.

### Where sleeping meets change detection

Change detection (ADR-0018 §4) stamps each chunk's component columns with a monotonic world version
and lets a query visit *only* chunks written since a caller's checkpoint. Compose that with
write-back skipping asleep bodies and the win is structural: **a settled world stamps nothing**, so a
change-tracking consumer — GPU upload, editor live-sync (M9), replication (M11) — does zero work for
it. Work tracks what *moved*, not world size. This is why the sleeping bookkeeping of M7.5 and the
version stamps of M7.6 are really one feature seen from two modules.

### Proofs

`tests/physics/sync_test.cpp`, pure CPU and structural on the public seams: `reconcile()` binds an
intent entity to a body placed at its `WorldTransform` and is idempotent; write-back moves an awake
body's transform and a `for_each_changed` consumer sees exactly it; a static body never stamps;
despawning an entity and dropping a `RigidBody` both unbind and destroy the body (roster cleanup).
The headline mirrors the M7.5 sleeping proof through the bridge: a box settles and sleeps, after
which a further tick stamps **nothing** (`for_each_changed` reports zero), and waking it brings back
exactly its chunk. A determinism case drives the same scene through `PhysicsSync::step` twice and
gets a bit-identical `world_hash()` and identical entity transforms — the ADR-0026 contract holding
across the ECS seam. The ECS mechanism itself is proven separately in
`tests/ecs/change_detection_test.cpp` (version monotonicity, per-column stamping, chunk skip,
`Schedule::run` advancing the epoch, and `par_for_each_changed` matching the serial form).

## Scene queries & external impulses (M7.7)

Simulation is only half of a physics core; gameplay needs to *ask* the world what is where and to
*push* on it. M7.7 adds both through the seam, reusing machinery already built.

### Raycast: one BVH, several customers

A raycast is the workhorse — hitscan weapons, line-of-sight, mouse picking (M9), AI probes, the
"what's under the crosshair" the playground fires along. The naive version tests the ray against
every body; instead `raycast` descends the ray through the **same dual AABB trees the broadphase
uses** (`aabb_tree.hpp::query_ray`), visiting only the O(log n) nodes the ray's slab test reaches,
and runs the exact ray-vs-shape test (`scene_query.hpp`) on just those candidate leaves. The nearest
survivor across both trees wins. The exact tests are analytic per primitive: ray-vs-sphere is a
quadratic; ray-vs-box rotates the ray into the box's local frame and runs a slab test (so an
*oriented* box is handled without a special case — the returned normal is the entered face rotated
back out); ray-vs-capsule is the union of an infinite cylinder clamped to the segment and two
end-cap spheres, each accepted only on its outer hemisphere. `RayHit` reports the body, world-space
point and outward normal, and the distance along the (normalized) direction. A `QueryFilter` selects
by motion class, which — because the broadphase already keeps statics and movers in separate trees —
is just "skip a whole tree," not a per-body test.

`overlap_sphere` is the volume query (an explosion's affected set for M8, a trigger pre-check): a
broadphase AABB query culls candidates, an exact sphere-vs-shape test confirms each, and the result
is returned in **canonical slot order** so it never leaks the hash/traversal order into something the
game or replication sees.

### Impulses: the gameplay push

`apply_impulse(body, J, point)` changes a dynamic body's linear velocity by `J/m` and its angular
velocity by `I⁻¹(r×J)` — using the *exact same* world-space inverse-inertia operator
(`apply_inv_inertia`, `R·diag(I_body⁻¹)·Rᵀ`) the contact solver applies, so an explosion's shove and
a resting-contact impulse rotate a body by identical math. It **wakes** the body (an impulse to a
sleeper would otherwise be ignored until the next contact), and no-ops on an immovable
(static/kinematic, zero inverse mass) or stale id. `apply_central_impulse` is the through-the-COM
shortcut with no angular term. This is all a projectile or a blast needs; forces/torques accumulated
across a step are a later addition if a system needs them (noted, not built).

### Proofs

`tests/physics/query_test.cpp`, pure CPU on the seam: a raycast battery (sphere head-on with exact
point/normal/distance; an axis-aligned box face; a **rotated** box proving the OBB path, not the
AABB; a capsule side and end-cap; clean misses for "above" and "pointing away"; the nearest of two
bodies; `max_distance` bounding a cast short; the motion-class filter picking floor vs. ball);
`overlap_sphere` finding exactly the planted set in canonical order; and impulses (central ⇒ `J/m`
with no spin; off-centre ⇒ spin about the expected axis; an impulse wakes a sleeper; a static body
ignores one). Green under dev + ASan/UBSan + TSan.

## Contact & sleep events (M7.9)

A core that only *moves* bodies is half a core; gameplay has to learn what happened. Destruction
(M8) is the archetype consumer: a wall takes damage proportional to how hard it was hit, so the sim
must report every contact and the impulse it carried. M7.9 adds that reporting as **buffered,
deterministic events** (`events.hpp`) — data the game reads *after* a step, not callbacks fired
*during* one.

### Data, not callbacks — and why that matters here

The obvious API is a listener the solver calls on each contact (Jolt's `ContactListener`). We
deliberately do the opposite and accumulate events into a list read after `step()` returns. Three
reasons, all specific to this engine:

- **The solve is parallel.** From M7.5 the per-island solve runs on worker threads. A callback would
  fire from whichever worker owns the island, in nondeterministic order, into game code that would
  then need its own locking — exactly the re-entrancy-and-ordering swamp buffering avoids.
- **Determinism is a contract.** M11 replicates and replays destruction against a "damage → detach
  is a pure function" promise, and damage is a function of the event stream. So the stream must be a
  pure function of the tick — same order, same payloads, every run and every thread count. A list
  built in canonical order in the sequential tail is; a set of thread-timed callbacks is not.
- **Consumption is naturally deferred.** The fixed tick (ADR-0023) already separates simulation from
  the systems that react to it. Events want the same seam: produced by the step, drained by the game
  when it is ready. The output is **double-buffered** — the tick fills a back buffer and swaps it to
  front — so a consumer always reads a stable snapshot of the completed tick.

Everything is emitted in `step()`'s **sequential tail**, after the parallel `parallel_for` has
joined: the events are a pure *read* of the just-solved state. Nothing there writes a body, so the
world hash and the cross-thread determinism of M7.5 are untouched — and the event stream is itself
bit-identical for any worker count, because it derives only from the canonical manifolds and the
(thread-count-independent) solved impulses and sleep decisions.

### Contact events: the began/persisted/ended lifecycle

Each tick, every touching body pair with a dynamic member produces one `ContactEvent` carrying the
pair (in canonical `a<b` order), a representative world point (the deepest manifold point), the
`a→b` normal, and — the payload that matters — the **total normal and friction impulse the solver
exchanged over the pair this tick**. Impulse is `∫F dt`; it *is* the "how hard" a damage model
wants, and it comes straight off the solved manifold (summed over its points). A `phase` tags the
contact `Began` (new this tick — the impact, where damage usually peaks), `Persisted` (a sustained
or resting contact), or `Ended` (separated this tick — impulses zero, nothing was exchanged). The
lifecycle mirrors Jolt's added/persisted/removed and PhysX/Unity's enter/stay/exit, so the familiar
damage-on-impact / sustained-crush / released patterns map straight on.

Immovable pairs (no dynamic member) are skipped entirely: they exchange no impulse, so there is
nothing to report and no lifecycle to track.

### The merge, and why no hash map

Classifying a pair means asking "were these two touching last tick?" The warm-start cache already
knows — but it is an *unordered* map and stores only feature ids and impulses, not the point/normal
an `Ended` event wants. So the event system keeps its own witness: one record per contacting pair,
in two vectors (`cur` this tick, `prev` last tick) kept **sorted by the pair key**. Because manifolds
already arrive in canonical order, `cur` is built already sorted, and a single linear **merge** of
`cur` against `prev` classifies every pair — key in both → `Persisted`, only in `cur` → `Began`, only
in `prev` → `Ended` — and emits them in canonical order with *no hash map and no per-tick sort*. The
two vectors swap each tick, reusing their capacity. (An `Ended` record is last tick's, so it carries
that pair's final point and normal; a body it names may since have been destroyed — the event honestly
reports the stale id, and a consumer can `is_alive`-check it.)

### Silence when settled

A settled pile should cost nothing *and say nothing*. A pair whose every dynamic member is asleep is
recorded **present but suppressed**: it emits no event, yet it stays in `cur` so it is not falsely
reported as `Ended` (an asleep pair never separated). The rule — "emit iff some dynamic participant
is awake" — is exactly the solver's active-island gate, so a contact event fires precisely when the
contact was actually solved. A box resting on the floor keeps emitting `Persisted` (with its
`m·g·dt` support impulse) until it sleeps, then goes silent on the very tick it deactivates — the
same tick a `Slept` event explains why.

### Sleep events

`SleepEvent` reports a body that deactivated (`Slept` — the basis for M8's `DebrisSettled`) or that
the simulation reactivated (`Woke` — a faller landing on and rousing a sleeping stack). Both are
computed by snapshotting each body's `asleep` flag at the top of `step()` and diffing at the tail,
so exactly the transitions *this step caused* are reported, in dense body order. A wake caused by an
explicit `wake_body()`/`apply_impulse()` between steps is deliberately **not** evented: the caller
performed it and already knows, and reporting it would blur "the world changed on its own" with "I
changed the world." Islands sleep as a unit (M7.5), so a settling stack emits a `Slept` for every
member on the tick it sleeps.

### What is deferred: triggers

The brick's original name was "contact/**trigger**/sleep events," and the trigger third is
deliberately left out. A trigger (sensor) is a body that *detects* overlap but generates no contact
response — a distinct concept requiring a sensor flag on bodies, a solver branch that skips
constraint generation for sensor pairs, and enter/exit tracking. None of that is in M7's shipped
shape/body scope, and nothing consumes it yet: M8 damage rides **contact** events, not triggers. So
by the same measure-first discipline ADR-0026 applies to the private AABB tree (generalize when a
second, measured consumer appears), triggers wait for the first gameplay volume that needs one; the
overlap machinery they will reuse (broadphase + `sphere_vs_shape`/manifold overlap) already exists.

**The deferred design, pinned so the retrofit stays cheap.** The real consumer is the first system
that needs enter/exit *lifecycle* — an M9 editor kill-zone, an m11.3 capsule mover — not a one-shot
volume test: a blast radius or an explosion's affected set is a single-tick question that
`overlap_sphere` already answers, and a persistent sensor body would only make it more expensive. So
triggers are specifically the *persistent presence* tool. When that consumer lands, the shape is
small and known: a `bool is_sensor` on `BodyDesc` (orthogonal to `MotionType`, so a *dynamic* sensor
is legal), one SoA column, a `trigger_events()` accessor, and a `TriggerEvent { BodyId sensor;
BodyId other; TriggerPhase phase; }` where `phase` is Entered/Stayed/Exited — presence only, no
point/normal/impulse (a sensor reports *that* it was entered, not *how hard*). The one load-bearing
change is at constraint prep (`step()` stage 3): a sensor pair is detected by the narrowphase exactly
as today but generates **no constraint** — and everything else falls out for free, because islands
are built from the constraint list, so a sensor pair never merges islands, never enters the parallel
solve, and never keeps a body awake. The began/persisted/ended merge in the event tail is already
enter/stay/exit; a second `cur`/`prev` record stream clones it. **The one trap to record now:** the
contact tail's "skip a pair with no dynamic member" rule is *wrong* for triggers — a kinematic
character walking into a **static** kill-zone has zero total inverse mass and would be silently
dropped. The trigger inclusion rule is instead "a sensor member plus at least one **non-static**
member." (Recorded from an M7.9 design consult; no ADR — ADR-0026's deferred register already
carries the decision.)

### Determinism, preserved

Events add no shared writes to the parallel region and no unordered iteration to the sim path: they
are built in the sequential tail from the canonical manifolds, by a merge of two key-sorted vectors,
in dense order for sleep. `world_hash()` is unchanged by their addition, and the event stream hashes
identically whether the islands solve sequentially or across any number of workers.

### Proofs

`tests/physics/events_test.cpp`, pure CPU and structural: a landing box reports `Began` then
`Persisted` for the canonical (floor, box) pair with an upward normal, and at rest the event's total
normal impulse equals the same `m·g·dt` the solver test reads from the cache — the payload tied to a
closed form; a settling body emits `Slept` exactly once and the pile then goes silent (no contact or
sleep events while asleep); launching a rested body off the ground produces an `Ended` (zero impulse)
and no lingering events once airborne; a box dropped on a sleeper emits a `Woke` for the roused body;
and the headline — a multi-island scene stepped 400 times yields one event-stream hash shared by the
sequential path and by 1-, 2-, and 4-worker job systems (the test that would catch any read of the
impulses from inside the parallel region). Green under dev + ASan/UBSan + TSan.
