# Physics core — design note (M7)

Companion to `engine/physics` (M7, ADR-0026). This note is the running design record of Rime's
own rigid-body core; it grows a section per brick, in the order the bricks land. The math
derivations live separately in [`docs/math/rigid-body-dynamics.md`](../math/rigid-body-dynamics.md);
this note covers the *systems* reasoning — data layout, algorithms, and the trade-offs behind them.

Current coverage: the world seam and body pool (M7.1), the broadphase (M7.2), the narrowphase
(M7.3). The impulse solver (M7.4), islands and the parallel step (M7.5) will extend it as they ship.

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
