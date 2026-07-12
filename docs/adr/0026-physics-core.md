# ADR-0026: The physics core — an own rigid-body engine, multicore-first

- Status: Accepted
- Date: 2026-07-12

## Context

M7 gives Rime `engine/physics`: rigid bodies, collision, queries, stepped on the job system
(ROADMAP "done when": *objects fall/collide/stack; raycasts hit; runs parallel to the frame*).
The one decision the rest of M7 — and M8 destruction, which stands on it — must cite is **what the
core *is*: do we integrate a mature third-party solver (Jolt is the obvious candidate, MIT, on Conan,
designed for exactly our job-system adaptation), or grow our own?**

The forces:

- **Destruction is physics at scale, and it is our headline** (VISION §3/§5). The core is not a generic
  dependency we adapt to; it is the substrate a Frostbite-class destruction system is *shaped around*:
  many small, fast, short-lived debris bodies; convex-hull fracture parts + compound islands;
  contact events carrying impulse (they drive damage); CCD for projectiles vs thin parts; same-binary
  determinism (M11 replicates destruction *events* against a "damage → detach is a pure function"
  contract). A core we own can be shaped *exactly* for this; a core we integrate, we adapt to.
- **VISION principle #1 — power beats portability**, and **#3 — the code teaches.** The engine is a
  textbook; a rigid-body solver you can *read* — broadphase, GJK/EPA, sequential impulses, islands,
  sleeping, CCD — is the centerpiece a learner comes for. Wrapping Jolt teaches *integration* honestly;
  building the core teaches *the physics*. For this project, the second is the point.
- **The honest cost.** An own core is the biggest single-milestone build the project has attempted
  (broadphase + narrowphase + solver + islands + sleeping + CCD + the geometry). The prior plan named
  this plainly. The answer is *cut scope, not corners* (the deferred register below), and a ladder whose
  hard bricks (solver, narrowphase, geometry) are de-risked by structural proofs from their first commit.
- **The platform reality (a gift here).** Rigid-body simulation is pure CPU — so **every M7 proof runs
  GPU-free on lavapipe** on all CI OSes + ASan/UBSan + TSan. Determinism is provable *bit-exactly* in CI,
  which for an own solver is the difference between "seems stable" and "is correct."
- **The seams the core must leave.** The core is not destruction's foundation only — it is the universal
  simulation substrate for lighting invalidation (M10), particles/fluids (Tracks FX/FL), and audio. Those
  consumers need *seams*, designed now, not a shared solver.

## Decision

**Rime builds its own rigid-body physics core behind `engine/physics` — no third-party solver.** The
module is a static lib (`rime_physics`) with PUBLIC deps `rime::core` + `rime::ecs` only; collision
geometry enters as point/index spans, so the module is **GPU-free by construction** and low in the layer
cake. All broadphase/narrowphase/solver internals live under `src/` (PRIVATE) — nothing above the seam
sees them (the RHI-seam discipline; there is simply no backend to hide, only our own internals). Runtime
state is **data-oriented SoA** in a `BodyPool` (generational `BodyId`, swap-remove keeps arrays dense →
churn-safe, which the M8 detach storm needs).

### The algorithm suite (chosen; each brick derives its math in `docs/math/`)

- **Broadphase — dual dynamic AABB trees** (static + dynamic, fat AABBs; serial mutation of only moved
  bodies, parallel read-only queries). It absorbs 500-body insertion storms at O(log n) and *is also* the
  ray/sweep/mesh-midphase engine — one structure, four customers. (Incremental SAP dies on
  debris-on-a-plane; grids die on the static-mesh-vs-10cm-part size heterogeneity.)
- **Narrowphase — GJK + EPA + reference-face-clipping manifolds** (feature ids, 4-point reduction,
  persistent cache for warm-starting). One support-function path collides *every* convex shape including
  arbitrary fracture hulls; SAT would be a second large algorithm for no extra coverage. Analytic
  fast-paths only for sphere/capsule.
- **Solver — sequential-impulse PGS**, warm-started from the persistent manifold, Coulomb friction as a
  2-axis pyramid, restitution as a velocity bias, penetration recovery by a **separate non-linear
  Gauss–Seidel position pass (split-impulse family) — not Baumgarte** (Baumgarte injects energy, which is
  debris that never sleeps). Fixed 8 velocity / 2 position iterations.
- **Parallelism & determinism — solve across islands** (connected components over the contact graph;
  static/kinematic bodies never merge islands), **strictly sequential within an island.** Islands share no
  dynamic body, so each island's result is a pure sequential function of its stably-sorted inputs ⇒
  **bit-identical regardless of thread count or scheduling.** Debris = many small islands = naturally fine
  parallel grain exactly when body counts spike. This paragraph is the milestone's determinism thesis.
- **Integration — semi-implicit (symplectic) Euler**; quaternion angular integration with world-space
  inverse inertia; the gyroscopic term is dropped v1 (explicit gyro is unstable; documented).
- **CCD — speculative contacts**, per-body opt-in flag. It folds into the solver (extended contact
  distance + approach-velocity clamp) — no TOI event queue, no mid-step re-simulation — so the determinism
  argument stays intact. The bullet-vs-5 cm-wall case is its sweet spot.
- **Islands & sleeping** — island-granular sleep (an island sleeps only when every member is a candidate);
  sleeping bodies skip integration, pair generation, and write-back; **sleep/wake are events**
  (`DebrisSettled` is built on them).
- **Shapes v1** — sphere / box / capsule / convex-hull / static-triangle-mesh / **compound** (static *and*
  dynamic — a hard M8 requirement for multi-part islands). Analytic + polyhedral-integral mass properties.

### Determinism scope

**Same-binary reproducibility** (tests, seeded destruction): world-state hash (`core::fnv1a_64` over exact
position/orientation/velocity bytes in body-index order) is asserted identical **across runs and across
{1,2,8,16} thread counts.** This is *not* cross-platform/cross-compiler lockstep (an explicit non-goal —
the 07-02 decision). Preconditions the ADR pins: no unordered iteration in the sim path; fixed iteration
counts (no convergence early-outs); **no fast-math** (verified absent from the build — CI keeps it absent).

### The core as a universal substrate — seams designed now, built later

M7 delivers rigid bodies only. The seams for every downstream are named here and in
`docs/design/simulation-tick.md` (m7.6), cheaply:

- **Destruction (M8):** contact events with impulse + point + normal; body create/destroy with pose +
  inherited velocity; compound/hull shapes; the CCD flag; sleep events; `WorldStats`. (M8 event payloads
  should additionally carry a world-space AABB — an ask into the m8.0/m8.4 plans.)
- **Lighting (M10):** *no new physics API* — it consumes **ECS change-detection on `WorldTransform`**
  (awake-only write-back ⇒ a settled debris field dirties nothing ⇒ zero SDF/shadow re-stamps) plus the
  destruction event stream.
- **Particles/fluids (Tracks FX/FL):** **batched, parallel-safe queries** callable from sim-phase jobs; a
  **pre-step force/impulse-accumulation API** on bodies (buoyancy); spawn-from-events; `BodyState` bulk
  reads. Bulk 10⁴–10⁵ particle collision is the **GPU SDF** (M10.4), *not* the physics tree — stated so
  the broadphase is not mis-designed for a customer it should not serve.
- **Audio:** the event channel (impulse → intensity) + batched raycasts (occlusion).

**Two calls made explicitly, not silently:** (1) the AABB tree stays **module-private in M7** — promoting
it to a shared engine facility is generalizing with one consumer, against measure-first; the trigger is a
second in-repo consumer with a measured need. (2) The unified simulation tick is a **convention, not new
machinery** — the existing `ecs::Schedule`'s declared-access phases already order it; m7.6 documents the
canonical order (fluids-coupling → physics step → destruction → secondary sims → fan-out).

### A load-bearing prerequisite this milestone lands: ECS change detection

ADR-0018 §4 specifies per-chunk change-detection version stamps, but they are **not implemented**
(`engine/ecs/{chunk,archetype,query,world}.hpp` carry no version fields). Physics write-back is their first
honest consumer ("sleeping bodies must not dirty change-detection"), so **m7.6 lands change detection as
public `engine/ecs` surface** (a world version ticked per `Schedule::run`, per-chunk-per-column write
stamps, a stamping write accessor, a `changed_since(V)` query filter) — *not* physics-internal bookkeeping.
It is load-bearing for M9 (live inspector sync), M10 (SDF/shadow dirt-tracking — the walls-fall GI assert
rides it), and M11 (replication deltas).

*Rejected:* **integrating Jolt behind the seam.** It is the strong engineering-pragmatic choice
(MIT/Conan, a `JobSystem` interface built for our adaptation, documented determinism, warnings-clean) and
the honest counterfactual: it would ship a robust solver in a fraction of the bricks. We decline it because
Rime's two top principles point the other way — the core is where *power-first* and *code-as-textbook*
matter most, and a solver we own is shaped for destruction rather than adapted to it. The door back to Jolt
stays open *behind the seam* if the own core's cost ever outruns its value; the seam is what makes that
reversible. (Verified at kickoff: `joltphysics/5.2.0` is MIT and on Conan Center — so the fallback is real,
not theoretical.)

## Consequences

- **The M7 ladder is 12 bricks** (m7.0 this ADR → m7.11 the `samples/09-physics-playground` proof), re-cut
  from the prior Jolt-shaped 7-brick plan. The DAG lets the query line (m7.7) and shapes line (m7.9)
  interleave after the narrowphase brick, hedging the schedule against the solver (the one hard brick)
  soaking. The brick list enters `docs/ROADMAP.md` with this ADR (house rule: the `.0` brick lands the
  ladder).
- **Every M7 proof is GPU-free and structural** (analytic rest heights, energy/momentum bounds,
  determinism hashes) on all CI OSes + ASan/UBSan + TSan — `rime_physics_tests` joins the TSan net (the
  scheduler/solver is a threading surface). Perf numbers (Release, this 16-core box) are recorded in PRs.
- **Deferred, each with a home** (the register M7 bricks and the backlog cite): TGS/soft-constraint solver
  mode (M12 stack-quality review) · implicit gyroscopic integration · conservative-advancement CCD for
  fast-*spinning* thin bodies · incremental islands + parallel tree mutation + hill-climbing support
  functions (measured-need optimizations) · scaled collision shapes (v1 ignores ECS scale, warned at bind)
  · joints/motors/character controller (m12.0, with vehicles/ragdolls; a capsule mover still ships at
  m11.3) · soft bodies/cloth/fluids (own modules — Track FL for water) · dynamic mesh-vs-mesh (never —
  convex decomposition via the M8.1 cook is the answer) · physics interpolation into render alpha (m12.0's
  look review) · cross-platform determinism (non-goal).
- **M8 inherits a firm contract**: contacts-with-impulse, hull/mesh/compound shapes, CCD, sleep events,
  `WorldStats`, and same-binary determinism — every one mapped to an M7 brick.
- **The dependency direction stays clean**: `engine/physics` depends only on `core` + `ecs`; destruction,
  render (lighting), and the effects modules depend on *it* (and on the ECS/event/cook seams), never the
  reverse.
