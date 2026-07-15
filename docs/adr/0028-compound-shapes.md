# ADR-0028: Compound collision shapes — one body, many convex children

- Status: Accepted
- Date: 2026-07-14

## Context

ADR-0026 names the compound a **hard M8 requirement** ("multi-part islands"; "Shapes v1 — sphere /
box / capsule / convex-hull / … / **compound** (static *and* dynamic)"), and ADR-0027 names it the
explicit next brick, "built directly on `HullId`". The destruction model needs it on day one: an
intact destructible is ONE rigid body whose collision shape is *several* convex parts (the fracture
cells, each a primitive or a hull, each at a local pose); when it breaks, parts detach into
separate bodies. M7.12 lands that shape.

Two questions dominate, and they are separable:

1. **Storage** — where does a variable-length child list live, given that `ShapeDesc` must stay a
   flat, trivially-copyable POD? This is ADR-0027's question again, and it gets ADR-0027's answer.
2. **The multi-contact model** — the hard one. Everything from the narrowphase down assumes **one
   contact manifold per body pair**: the persistent warm-start cache is keyed by
   `(slot_a << 32) | slot_b`, one `ManifoldCacheEntry` (≤ 4 points) per key; the M7.9 event tail
   builds one lifecycle record per pair. A compound breaks the assumption physically: a
   dumbbell-shaped compound standing on the floor touches in **two places** a metre apart, and no
   single 4-point patch can represent both (the solver would support one foot and let the body tip
   over the other). A body pair must be able to carry **several manifolds** — one per touching
   child pair — *without changing behaviour for non-compound bodies* (every prior suite must pass
   unchanged, the same backward-compatibility bar the hull brick set with its identity-compose).

The forces on the contact model:

- **Determinism (ADR-0026).** Whatever enumerates child pairs must do so in a fixed, canonical
  order; cache keys and feature ids must be pure functions of registration-time indices; nothing
  may iterate an unordered container in the sim path.
- **Warm starting must survive per child.** A compound resting on the ground keeps distinct
  contact regions alive across ticks; if their cached impulses were pooled under one pair key, one
  region's points would steal or shadow another's (same-id collisions across children), and stacks
  would buzz. Feature ids and cache keys must incorporate the child indices.
- **The solver and islands already speak body language.** A constraint references two *body*
  dense indices plus world-space contact points; an island unions *bodies*. Contacts found at a
  child naturally apply to the parent (the lever arm is measured from the parent's COM, which is
  the body position); a pair contributing several manifolds just contributes several constraints,
  and union(a, b) is idempotent. Neither needs to know children exist.
- **Compounds are a minority of bodies.** In the M8 detach storm the population is hundreds of
  simple hull debris bodies and a handful of intact compounds. The design should tax compounds,
  not every body.

## Decision

### Storage: a world-owned compound store (the ADR-0027 pattern, verbatim)

`PhysicsWorld::register_compound(const CompoundDesc&)` takes a child list — each child a
`ShapeDesc` (primitive or `ConvexHull` by `HullId`; **not** another compound, rejected in v1) plus
a local pose — validates it, derives everything once, and returns a small `CompoundId`.
`ShapeDesc` gains `ShapeType::Compound` plus that id and stays a flat POD. The store is world-owned,
immutable, append-only (ids are registration order — deterministic); child geometry and poses live
under `src/compound.hpp`, invisible above the seam. One fracture *pattern* registers one compound;
every intact instance of that destructible references it by id — the same shape economy as hulls.

- **Mass properties by composition, uniform density.** v1 assumes one material across the children
  (exactly the fracture case — a wall's cells share its density), so each child's mass fraction is
  its volume fraction. Registration computes each child's volume (closed forms for primitives —
  the capsule as its v1 cylinder, consistent with its v1 cylinder inertia — and the stored integral
  for hulls), the combined COM (volume-weighted child COMs), and the combined inertia: each child's
  per-unit-mass inertia rotated into the compound frame (`R·diag·Rᵀ`, with `R` composing the child
  pose and, for hull children, the hull's principal rotation) plus the **parallel-axis** shift
  `|d|²·E − d·dᵀ` for its COM offset `d`, summed with the volume weights, then diagonalized by the
  existing fixed-sweep Jacobi (`docs/math/compound-mass-properties.md` derives it;
  `hull.hpp`'s machinery is reused, not duplicated).
- **Re-centred on the combined COM.** Stored child poses are shifted so the compound's COM is the
  local origin — the engine-wide "a body's `position` IS its COM" invariant holds for compounds by
  construction, exactly as ADR-0027 did for hulls (which promised compounds as "the honest home
  for deliberate COM offsets": author children anywhere; the store re-centres and reports the
  shift via `CompoundInfo::centroid`). A pleasant consequence of that same hull decision: every
  child shape's *own* stored form is already COM-centred, so a child's COM in the compound frame
  is simply its authored position — no per-child offset bookkeeping.
- **Like `register_hull`: validate and reject, never repair.** ≥ 1 child (one child is legal — a
  deliberate COM-offset shape), ≤ 256 children (v1 headroom for fracture cells; child indices
  travel in 16 bits, so the cap can grow ×256 without a format change), no compound children, hull
  ids that resolve in this world, non-degenerate child volumes and orientations. Null id on any
  violation; nothing stored on failure.

### The multi-contact model: narrowphase expansion (option A)

**One broadphase proxy per body, always** — a compound's proxy bounds the union of its posed
children. When a candidate pair involves a compound, `build_contacts` enumerates child-vs-child
sub-pairs (a non-compound body counts as one child) in **fixed lexicographic order**
(a's child index ascending, then b's), culls each sub-pair by the children's world AABBs (the v1
midphase — brute force is right at fracture-cell counts), runs the **existing** per-convex-shape
collision routine on the posed child shapes, and emits **one manifold per touching child pair**.
The manifold list stays a flat vector in canonical order — now lexicographic by (pair, child a,
child b) — so everything downstream stays deterministic by construction.

- **`Manifold` carries the child indices** (`child_a`/`child_b`, 0 for non-compound bodies).
  The solver applies the manifold to the *parent* bodies unchanged — contact points are world-space
  and lever arms come from the parent COM, which is exactly what a rigidly-attached child means.
  Islands union the parent bodies once per constraint, idempotently.
- **The warm-start cache is keyed per contact region**: `(body pair, child_a, child_b)` — the
  64-bit pair key plus a 32-bit packed child sub-key. Non-compound pairs use sub-key 0 and behave
  bit-identically to M7.3–M7.11. The cache map is still only ever *looked up*, never iterated, so
  its unordered-ness stays invisible to simulation order.
- **Feature ids fold in the child indices** (for pairs involving a compound only — non-compound
  id spaces are untouched, preserving every existing scene's warm-start behaviour bit-for-bit).
  Belt and braces with the cache sub-key: ids stay unique across regions even if a future
  consumer pools manifolds per pair.
- **Speculative CCD generalizes per child.** A compound is not convex, so it has no single support
  function — but each child is, so the existing shape-agnostic speculative path (M7.10) runs per
  sub-pair (with the child cull swept by `v·dt`, mirroring the proxy sweep). A CCD bullet is
  arrested at a compound wall exactly as at a simple one.
- **Queries iterate children**: raycast returns the nearest child hit (fixed scan, strict `<`, so
  ties keep the lowest child index); `overlap_sphere` ORs over children. Both inherit the existing
  per-shape exact tests.

**Contact events report per contact region** (`ContactEvent` gains `child_a`/`child_b`; the
lifecycle merge keys on (pair, children)). This is the variant that serves M8 damage: a hit on a
destructible must name *which part* took the impulse — that is the cell the fracture detaches. A
consumer wanting per-pair totals can sum regions; the reverse direction (recovering the part from
an aggregate) is impossible. Cost, stated honestly: a compound sliding across a floor can Begin/End
individual regions while the body pair stays continuously touching — more events, each still
deterministic, each individually meaningful to damage. Non-compound pairs emit exactly the M7.9
stream (one region, sub-key 0).

*Rejected — **(B) sub-proxies** (one broadphase proxy per child, tagged with parent + child
index):* cleaner narrowphase (pairs arrive child-level) and a tighter broadphase for sprawling
compounds, but it breaks the proxy↔body 1:1 model that `create_body`/`destroy_body`, the CCD
proxy sweep, the post-solve refit, `broadphase_aabb()`, and both query descents all assume — every
proxy site grows a fan-out loop and a proxy→(body, child) indirection, pairs need collapsing to
parent level for islands/events anyway, and the broadphase's pair keys (packed slot ids) would need
widening. That is a much larger, riskier diff for a benefit that only materializes when compounds
are both numerous and sprawling — the opposite of the M8 population. Revisit (it composes cleanly
with the tree we have) if profiling ever shows compound AABB fatness dominating pair counts.
*Also rejected:* **widening `Manifold` to N points** (a variable-length manifold poisons the POD
pool model and the solver's fixed-size constraint blocks; several 4-point manifolds are the same
information in the shape the solver already eats); **hashing child indices into the existing
64-bit cache key** (a hash collision would silently cross-wire two regions' warm-start impulses;
widening the key is exact and costs nothing measurable).

## Consequences

- M8 gets its shape: an intact destructible is one compound body (static wall or dynamic loose
  piece); on fracture, children detach into hull bodies that were registered once per pattern.
  Contact events name the child hit — the damage-to-part mapping is direct.
- A body pair may now carry several manifolds; every stage that consumed "the pair's manifold" was
  audited for the plural: cache (region-keyed), events (region lifecycle), solver/islands (already
  plural-safe by construction). Non-compound worlds run byte-identical paths — the prior suites
  pass unchanged as the regression witness.
- The compound's broadphase AABB is the union bound — fatter than per-child proxies for sprawling
  compounds, so some sub-pair culls run on pairs a child-level broadphase would never have
  reported. Accepted for v1 (see rejected (B)); the child-AABB cull bounds the waste to O(children)
  per false pair.
- Child enumeration is O(children_a × children_b) AABB tests per compound pair. At fracture-cell
  counts (tens) this is noise; a **child BVH midphase** is the named follow-up if profiling ever
  says otherwise (measured need, ADR-0026 discipline).
- **Deferred, each with a home:** **nested compounds** — rejected at registration in v1; if the M8
  cook wants authored nesting, the home is flatten-at-register (compose poses into one flat child
  list), which changes no runtime type; **child BVH midphase** — with the first measured compound
  hot spot, alongside the static-mesh midphase (same tree); **compound & hull unregister /
  refcounting** — together, when M8 fracture-pattern lifetime becomes real (streaming levels);
  **ECS `Collider` compound reference** — with asset-level shape identity from the M8 cook (same
  reason as the hull's, ADR-0027); **per-child materials** (friction/restitution stay per-body) —
  with the M8 material system, when a destructible's parts first need to differ; **compound CCD
  via conservative advancement** — per-child speculative contacts are the v1, inheriting M7.10's
  posture and limits.
