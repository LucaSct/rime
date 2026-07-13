# Physics core — design note (M7)

Companion to `engine/physics` (M7, ADR-0026). This note is the running design record of Rime's
own rigid-body core; it grows a section per brick, in the order the bricks land. The math
derivations live separately in [`docs/math/rigid-body-dynamics.md`](../math/rigid-body-dynamics.md);
this note covers the *systems* reasoning — data layout, algorithms, and the trade-offs behind them.

Current coverage: the world seam and body pool (M7.1), the broadphase (M7.2). Narrowphase (M7.3),
the impulse solver (M7.4), islands and the parallel step (M7.5) will extend it as they ship.

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
