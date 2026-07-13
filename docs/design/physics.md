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
