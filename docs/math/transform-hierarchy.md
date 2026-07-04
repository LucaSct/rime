# The transform hierarchy — `world = parent · local`

Companion derivation for `engine/ecs/transform.{hpp,cpp}` (brick M4.5). Math-heavy bricks ship a
short note like this so the reasoning is legible, not just the code. The composition algebra of a
single `core::Transform` is derived in [quaternions-transforms.md](quaternions-transforms.md); this
note is about *chaining* transforms down a parent hierarchy and doing it in the right order, in
parallel.

## The placement of a thing is the placement of its parent, applied to its own

A scene is a forest. A turret sits on a tank; a barrel sits on the turret. We author each thing's
placement **relative to its parent** — the barrel is "0.5 m forward of the turret's pivot" — because
that is what stays true when the tank drives away. That authored, parent-relative placement is the
**local transform** $L$. What the renderer and physics need is the **world transform** $W$: where the
barrel actually is, in world space.

The relationship is a change of frame. A point $p$ given in an entity's local coordinates is, in its
*parent's* frame, $L\,p$ (apply the local placement). But the parent's frame is itself placed in the
world by the parent's world transform $W_{\text{parent}}$, so in world coordinates the same point is

$$W\,p \;=\; W_{\text{parent}}\,\big(L\,p\big) \;=\; \big(W_{\text{parent}}\, L\big)\,p
\qquad\Longrightarrow\qquad \boxed{\,W \;=\; W_{\text{parent}} \cdot L\,}$$

for every point $p$, so the world transform is the parent's world transform composed with the local
one. A **root** has no parent, i.e. its local placement is already world-relative, so $W = L$.

Composition $\cdot$ is exactly `core::Transform::operator*` (translate–rotate–scale composed as
$T R S$; see the core note). It is associative, which is what lets the recurrence telescope down a
chain: for barrel ⟵ turret ⟵ tank,

$$W_{\text{barrel}} \;=\; W_{\text{turret}}\cdot L_{\text{barrel}}
\;=\; \big(W_{\text{tank}}\cdot L_{\text{turret}}\big)\cdot L_{\text{barrel}}
\;=\; L_{\text{tank}}\cdot L_{\text{turret}}\cdot L_{\text{barrel}}.$$

(As in the core note, this decomposed TRS product is exact for uniform scale and an approximation
under non-uniform scale + rotation, which can introduce a shear a single TRS cannot store. Bake to a
`Mat4` when that matters.)

## Order is the whole problem: compute parents before children

The recurrence $W = W_{\text{parent}}\cdot L$ has a data dependency: a child cannot be computed until
its parent's **world** transform exists. Evaluate the entities in the wrong order and a child composes
against a stale (last-frame, or zero) parent world.

Order them by **depth** $d$ — the number of ancestors — with roots at $d=0$:

$$d(e) \;=\; \begin{cases} 0 & e \text{ is a root}\\ d(\text{parent}(e)) + 1 & \text{otherwise.}\end{cases}$$

Then process depth by depth, $d = 0, 1, 2, \dots$. When we reach level $d$, every entity at level
$d-1$ — which includes every level-$d$ entity's parent, by the definition of depth — is already done.
So the recurrence's input is always ready, and correctness follows by induction on $d$.

Two facts make this the parallel-friendly choice, which is the point of an archetype ECS:

- **Within a level, entities are independent.** Each writes its own `WorldTransform` and only *reads*
  its parent's, which lives at a shallower, already-finished level and is not being written now. So a
  whole level updates with one `JobSystem::parallel_for` — no locks, no races. `propagate_transforms`
  does exactly this; the join between levels is the happens-before barrier that publishes level
  $d-1$'s writes to level $d$'s reads.
- **The common case is one level.** A flat scene (no parents) is all roots — a single fully-parallel
  pass of `world = local`, which `propagate_transforms` special-cases as a fast path (no depth
  bucketing at all). This is the shape of the M4.6 "100k entities in parallel" proof.

The number of sequential `parallel_for`s equals the tree's **maximum depth**, which is the irreducible
serialization: a depth-$k$ chain genuinely cannot be evaluated in fewer than $k$ dependent steps. The
width at each level — where the parallelism is — is unbounded.

## Cost, and what we deliberately left out

Building the levels is $O(n)$: gather the transform entities, memoize each one's depth by walking up
to a known ancestor (each entity resolved once; a malformed cycle is broken by a "computing" sentinel
so it degrades to a finite answer instead of looping), then bucket. The per-entity transform math —
the expensive part — is the parallel part.

We recompute **every** world transform each call. [ADR-0018](../adr/0018-ecs-storage-model.md) designed
per-chunk change-detection version stamps precisely so this pass could later **skip subtrees whose
locals didn't move** — dirty propagation, change-detection's intended first consumer. We defer that
until it is measured to matter (VISION / CLAUDE "measure before optimize"): recompute-all is correct,
trivially parallel, and its real beneficiaries (the M9 editor's live sync, M11's replication deltas)
land later. The seam is designed; the optimization waits for a profile.
