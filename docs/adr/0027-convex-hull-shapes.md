# ADR-0027: Convex hull collision shapes — a world-owned hull store

- Status: Accepted
- Date: 2026-07-14

## Context

ADR-0026 names convex hulls a **hard M8 requirement**: destruction fractures walls into convex
parts, and every part must be a first-class collision shape ("Shapes v1 — sphere / box / capsule /
convex-hull / …", "collision geometry enters as point/index spans"). M7 shipped the three
primitives; M7.11 ("shapes II", the ROADMAP fast-follow that "wants a shape-storage ADR first")
now lands the hull. The decision this ADR pins is **where variable-length hull geometry lives and
how a body refers to it** — the primitives never posed the question, because a sphere/box/capsule
fits whole inside the flat `ShapeDesc` POD.

The forces:

- **`ShapeDesc` must stay a trivially-copyable POD.** It is stored per body in the SoA pool,
  copied on body create/swap-remove, embedded in `BodyDesc`, and is the shape currency of the
  whole seam. A hull is `N` vertices + `F` faces — unbounded. Owning pointers or inline arrays in
  `ShapeDesc` would poison every copy site and every POD assumption (reflection, future
  serialization).
- **Hulls are shared, bodies are churned.** One fracture pattern produces a fixed set of part
  hulls that *hundreds* of debris bodies instantiate over a level's life (the same wall breaks the
  same way every time — determinism demands it). Geometry wants to be registered once and
  referenced many times; bodies want to stay cheap to create and destroy (the M8 detach storm).
- **Deriving hull physics is expensive relative to a body spawn.** Mass properties (a polyhedral
  integral + a 3×3 eigendecomposition), face planes, and validation are cold-path work that must
  not run per body — and must run *somewhere* exactly once.
- **The seam discipline (ADR-0026).** Nothing above `PhysicsWorld` may see collision internals.
  Whatever the storage is, the hull's vertex/face representation must live under `src/`.
- **Determinism (ADR-0026).** Hull identity, iteration order over hull features, and the derived
  quantities must be pure functions of the registration inputs — no pointer-order, no unordered
  containers.

## Decision

**Hull geometry lives in a world-owned hull store.** The caller registers a hull's geometry once —
`PhysicsWorld::register_hull(const HullDesc&)` — and gets back a small `HullId` handle;
`ShapeDesc` gains `ShapeType::ConvexHull` plus that id field and **stays a flat, trivially
copyable POD**. This mirrors the engine's asset registries (meshes/materials are ids into a store,
never inline data) and Box2D/Jolt practice (shapes are referenced, not embedded). Internals:

- **Authored input, validated — no runtime hull construction.** `HullDesc` is vertex + face spans
  (faces as per-face counts + concatenated, outward-wound indices — the CSR shape the M8 fracture
  cook naturally produces). Registration *validates* (≥ 4 vertices, 3..16 vertices per face,
  closed 2-manifold via directed-edge pairing, convex + outward via every-vertex-behind-every-face,
  positive volume) and returns the null `HullId` on any violation; it never repairs or
  reconstructs. Building a hull *from a point cloud* (quickhull) is a **cook-time concern (M8.1)**
  — a runtime solver has no business running an O(n log n) geometry algorithm with robustness
  edge cases mid-frame, and the fracture tool has to run it offline anyway.
- **Registration derives everything once**: outward unit face normals + plane offsets, the
  polyhedral mass properties (volume, centre of mass, full inertia tensor by signed-tetrahedron
  decomposition — `docs/math/polyhedral-mass-properties.md`), and the **principal axes** (Jacobi
  eigendecomposition of the inertia tensor). Bodies created from the hull just scale the cached
  per-unit-mass moments by their mass.
- **Stored vertices are re-centred on the centre of mass.** The engine-wide invariant "a body's
  `position` IS its centre of mass" (integrator lever arms, `apply_impulse`, solver anchors) is
  kept by construction: the store shifts the authored vertices so the COM is the local origin, and
  reports the shift (`HullInfo::centroid`) so the caller can place render geometry to match. The
  alternative — carrying a COM offset through the dynamics — would thread an extra transform
  through the integrator, solver, and every query for the benefit of exactly one shape type;
  compound shapes (deferred) are the honest home for deliberate COM offsets.
- **Inertia stays a body-space diagonal in the solver; a per-body principal-axis rotation joins
  it.** A general hull's inertia tensor is a full symmetric 3×3; diagonalizing it at registration
  keeps the solver's cheap `R·diag·Rᵀ` inertia application and its SoA layout, at the cost of one
  extra quaternion column (identity for primitives) composed on the fly (`q_body · q_principal`).
  Composing per use rather than caching per tick is deliberate: the NGS position pass rotates
  bodies *mid-pass* and re-reads their orientation, so a cached composite would go stale exactly
  where correctness matters.
- **The hull id resolves only inside the world.** `HullId` is opaque above the seam; vertices,
  faces, and planes live in `src/hull.hpp`, resolved to internal pointers by `world.cpp` before
  the narrowphase/queries run. The only public read-back is `hull_info()` — derived *physical*
  quantities (volume, centroid shift, per-unit-mass principal moments, principal rotation) for
  tooling, tests, and render alignment, not geometry.
- **Lifetime: hulls are immutable and live as long as the world.** The store is append-only in
  v1 — ids are registration order, so identical call sequences give identical ids (determinism).
  Levels own their worlds; a world's hulls die with it.

*Rejected:* **inline geometry in `ShapeDesc`** (breaks the POD, copies geometry per body);
**caller-owned spans referenced by pointer** (lifetime coupling across the seam — a dangling
fracture-tool buffer becomes a physics crash, and pointer identity is not deterministic);
**a global/static hull registry** (hidden shared mutable state across worlds — exactly the
threading guardrail CLAUDE.md bans).

## Consequences

- `engine/physics` collides arbitrary convex fracture parts: the hull's support function drops
  into the existing GJK/EPA path unchanged (the ADR-0026 bet — "one support-function path collides
  every convex shape" — pays off here), and reference-face clipping generalizes from the box
  special case to arbitrary hull faces (`docs/design/physics.md` §Convex hulls). Boxes keep their
  specialized fast path; box-vs-hull adapts the box as an 8-vertex hull view on the stack.
- Queries and CCD inherit hulls for free where they are support-function-generic (speculative
  contacts, overlap), plus a convex slab test for rays; M8 can shoot at debris from day one.
- One body-pool column is added (the principal-axis quaternion); `MassProperties` and the solver's
  diagonal representation are otherwise untouched, and primitive behaviour is bit-preserved
  (identity compose).
- A face is capped at 16 vertices (validated) so clipping runs in fixed stack buffers —
  allocation-free narrowphase, as before. Fracture cells are far below the cap in practice.
- **Deferred, each with a home:** runtime **quickhull** → the M8.1 fracture cook (registration
  will accept its output unchanged); **compound shapes** → the next shapes brick, built directly
  on `HullId` (a compound is a list of child shape refs + poses; its COM/inertia composition
  reuses this brick's mass-property machinery); **static triangle mesh + midphase** → with the
  destruction world-geometry brick (the AABB tree is already the planned midphase); **hull
  unregister/refcounting** → with compound, when fracture-pattern lifetime becomes real;
  **ECS `Collider` hull reference** → needs asset-level hull identity from the M8 cook (a raw
  world-local id does not serialize); until then hull bodies are created through the seam.
- The M8 contract sharpens: fracture cooks author vertex/face spans, register them once per
  pattern, and spawn debris by `HullId` — geometry upload cost is paid at pattern load, not at
  detach time.
