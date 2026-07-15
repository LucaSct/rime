// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/contact.hpp"
#include "rime/physics/events.hpp"
#include "rime/physics/query.hpp"

// The job system (rime::core) is BORROWED by the M7.5 parallel step. Forward-declared here so this
// seam header stays free of core's threading headers — only world.cpp pulls them in.
namespace rime::core {
class JobSystem;
}

// PhysicsWorld — the simulation container and the whole public seam of the module. Everything the
// rest of the engine touches is here; the broadphase, narrowphase, solver, and body storage live
// behind the pImpl, so nothing above the seam sees an internal header (the RHI discipline). Later
// bricks add queries (M7.7), events (M7.8), and a job-system/allocator-backed step (M7.5/M7.6) to
// this same class — the seam is expected to grow across the milestone, not be frozen at M7.1.
namespace rime::physics {

// Authored convex-hull geometry for PhysicsWorld::register_hull (M7.11, ADR-0027): vertices plus
// faces in CSR form — face f has face_counts[f] vertices, whose indices are consecutive in
// face_indices, wound OUTWARD (counter-clockwise viewed from outside the hull). The spans are only
// read during the register_hull call (the world copies what it keeps), so they may point at stack
// or scratch memory. The input must already BE a convex hull: registration validates (closed,
// convex, outward, 3..16 vertices per face, positive volume) and rejects — it never repairs, and
// it never *constructs* a hull from a point cloud (quickhull is the M8.1 cook's job).
struct HullDesc {
    std::span<const core::Vec3> vertices;
    std::span<const std::uint32_t> face_counts;  // one entry per face
    std::span<const std::uint32_t> face_indices; // concatenated per-face vertex indices
};

// Authored compound-shape children for PhysicsWorld::register_compound (M7.12, ADR-0028): each
// child a convex ShapeDesc (primitive or registered hull — never another compound, rejected in
// v1) posed in the compound's authored frame (shape.hpp, CompoundChildDesc). The span is only read
// during the call (the world copies what it keeps). Registration validates — child count in
// [1, 256], no compound children, hull ids that resolve in THIS world, non-degenerate child
// volumes/orientations — and returns the null id on any violation; it never repairs.
struct CompoundDesc {
    std::span<const CompoundChildDesc> children;
};

// Derived physical properties of a registered compound — the same read-back contract as HullInfo.
// Child shapes and poses are deliberately NOT exposed: they live behind the seam (ADR-0028), and
// the authoring side already has the authored data.
struct CompoundInfo {
    float volume = 0.0f; // m³ — the children's total (uniform density is the v1 mass model)
    // Combined centre of mass in the AUTHORED frame. register_compound re-centres the stored
    // child poses on it so a body's `position` IS the compound's COM (the engine-wide invariant) —
    // place visuals at position + rotate(orientation, authored_point - centroid).
    core::Vec3 centroid{0.0f, 0.0f, 0.0f};
    // Principal moments for a 1 kg body of this compound (I ∝ m at fixed shape), about the COM,
    // along the principal axes — composed over the children by the parallel-axis theorem
    // (docs/math/compound-mass-properties.md).
    core::Vec3 inertia_per_mass{1.0f, 1.0f, 1.0f};
    // Rotation from the principal-axis frame to the compound's local frame (identity when the
    // composed tensor is already diagonal — e.g. an axis-aligned symmetric arrangement).
    core::Quat principal_rotation = core::quat_identity();
    std::uint32_t child_count = 0;
};

// Derived physical properties of a registered hull — the read-back for tooling, tests, and render
// alignment. Geometry itself (vertices/faces) is deliberately NOT exposed: it lives behind the
// seam (ADR-0027), and consumers who need the render mesh already have the authored data.
struct HullInfo {
    float volume = 0.0f; // m³, of the authored solid
    // Centre of mass in the AUTHORED frame. register_hull re-centres the stored geometry on it so
    // that a body's `position` is the hull's COM (the engine-wide invariant every lever arm
    // assumes) — place visuals at position + rotate(orientation, authored_point - centroid).
    core::Vec3 centroid{0.0f, 0.0f, 0.0f};
    // Principal moments of inertia for a 1 kg body of this geometry (inertia scales linearly with
    // mass at fixed shape, so a body of mass m has moments m·inertia_per_mass), about the COM,
    // along the principal axes.
    core::Vec3 inertia_per_mass{1.0f, 1.0f, 1.0f};
    // Rotation from the principal-axis frame to the hull's local frame. Identity when the authored
    // geometry's inertia tensor was already diagonal (e.g. an axis-aligned box-shaped hull).
    core::Quat principal_rotation = core::quat_identity();
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    // Non-copyable: a world owns a simulation; copying it would alias body ids.
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Create a body from a description; returns a generational id valid until destroy_body().
    [[nodiscard]] BodyId create_body(const BodyDesc& desc);

    // Destroy a body. A stale/unknown id is a safe no-op (generational check).
    void destroy_body(BodyId id);

    // Read a body's motion state. Returns false (and leaves `out` untouched) for a dead/unknown id.
    [[nodiscard]] bool get_body_state(BodyId id, BodyState& out) const;

    // Whether `id` still refers to a live body.
    [[nodiscard]] bool is_alive(BodyId id) const noexcept;

    [[nodiscard]] std::size_t body_count() const noexcept;

    void set_gravity(core::Vec3 g) noexcept;
    [[nodiscard]] core::Vec3 gravity() const noexcept;

    // Advance the simulation by a fixed timestep — the full pipeline: integrate velocities
    // (gravity + damping), detect contacts (broadphase M7.2 → narrowphase M7.3, warm-started from
    // the persistent cache), partition into simulation islands (M7.5) and resolve each with the
    // sequential-impulse solver (PGS + friction pyramid + restitution) on the job system, integrate
    // positions, then an NGS position pass that recovers residual penetration without touching
    // velocities (deliberately not Baumgarte — ADR-0026). Solved impulses are committed back to the
    // contact cache for next tick's warm start; islands that come to rest go to sleep (M7.5). The
    // tick is deterministic to the bit for any worker-thread count (world_hash()). Called from
    // inside the app's fixed tick (ADR-0023), never per render frame.
    void step(float dt);

    // --- Broadphase (M7.2) ------------------------------------------------------------------
    // A candidate overlapping pair: the two bodies' fat AABBs overlap, so the exact narrowphase
    // (M7.3) should test them. Reported in canonical order (a's slot < b's slot) within a list that
    // is deterministically sorted, so replays and (from M7.5) thread-count changes see the same
    // pairs.
    struct Pair {
        BodyId a;
        BodyId b;
    };

    // Recompute the broadphase over the current body bounds and fill `out` (cleared first) with the
    // candidate pairs. Pairs where BOTH bodies are static are never reported (neither can move).
    // This is what M7.3's narrowphase will consume; it is exposed now so the broadphase is directly
    // testable against a brute-force oracle.
    void compute_pairs(std::vector<Pair>& out) const;

    // A body's current broadphase (fat) bounds. Returns false (and leaves `out`) for a dead id.
    [[nodiscard]] bool broadphase_aabb(BodyId id, Aabb& out) const;

    // Test/debug hook: the broadphase trees satisfy their structural invariants (every internal
    // node bounds its children; parent links are consistent).
    [[nodiscard]] bool validate_broadphase() const;

    // --- Narrowphase (M7.3) -----------------------------------------------------------------
    // Turn the broadphase candidate pairs into exact contact manifolds: analytic fast paths for
    // sphere/capsule pairs, GJK + EPA + reference-face clipping for boxes and convex hulls
    // (M7.11). Each manifold carries feature-id-tagged points whose accumulated impulses persist
    // across ticks (warm starting) — the solver's input. step() runs this same collision path
    // internally (M7.4); this method stays exposed for direct testing/inspection exactly like
    // compute_pairs, with one difference: no solver runs between the build and the cache commit,
    // so the impulses it persists are whatever the cache already carried.
    //
    // Fills `out` (cleared first) with one manifold per genuinely touching pair, in the
    // broadphase's canonical pair order, so the list is deterministic run to run. Advances the
    // persistent manifold cache, carrying each surviving contact's warm-start impulses forward by
    // matching feature id (generation-guarded so a recycled body slot never inherits stale state).
    void compute_contacts(std::vector<Manifold>& out) const;

    // How many contact points the most recent contact build (a step() or a compute_contacts())
    // matched — by feature id — against the previous tick's cache (i.e. were warm-started). 0 on
    // a fresh world's first tick. The narrowphase tests' witness that feature ids are
    // frame-stable, and from M7.4 the solver's warm-start hit rate.
    [[nodiscard]] std::uint32_t contacts_warm_started_last() const noexcept;

    // --- Islands, sleeping & the parallel step (M7.5) ---------------------------------------
    // Borrow a job system so step() solves independent simulation islands in parallel. Passing null
    // (the default) solves them sequentially. Either way the result is BIT-IDENTICAL: islands share
    // no dynamic body and immovable bodies are read-only in the solver, so an island's math is a
    // pure function of that island regardless of which worker runs it (the determinism contract,
    // ADR-0026; see world_hash()). The world does not take ownership — the engine owns the one job
    // system and hands it to each subsystem.
    void set_job_system(core::JobSystem* jobs) noexcept;

    // Enable or disable sleeping (on by default). A resting island deactivates so it costs nothing
    // to step; disabling immediately wakes every body — useful for a test that wants pure
    // continuous dynamics, or a scene where nothing should ever deactivate.
    void set_sleeping_enabled(bool enabled) noexcept;

    // Whether a body is currently asleep (deactivated): skipped by integration and the solver until
    // something wakes it — a moving body colliding into its island, wake_body(), or sleeping being
    // disabled. False for a dead/unknown id.
    [[nodiscard]] bool is_asleep(BodyId id) const noexcept;

    // Force a body — and, on the next step(), its whole island — awake. Call after teleporting it,
    // applying an impulse, or otherwise changing its state from outside the simulation, so a
    // sleeping body does not ignore the change. A stale/unknown id is a safe no-op.
    void wake_body(BodyId id) noexcept;

    // How many islands the most recent step() partitioned the world into — a determinism/behaviour
    // witness in the spirit of contacts_warm_started_last(). 0 before the first step().
    [[nodiscard]] std::size_t islands_last() const noexcept;

    // A 64-bit fingerprint of the full motion state (positions, orientations, linear and angular
    // velocities) of every body, in dense order. Two worlds that were stepped identically hash
    // identically iff every float matches bit for bit — the check the parallel step must pass for
    // any worker-thread count, and the hook that networked-destruction determinism and replay
    // validation build on. FNV-1a (core/hash.hpp): a fast exact-equality witness, not a
    // cryptographic digest.
    [[nodiscard]] std::uint64_t world_hash() const noexcept;

    // --- Scene queries & external impulses (M7.7) -------------------------------------------
    // Cast a ray and report the NEAREST body it hits within `ray.max_distance`, or return false
    // (out untouched) on a miss. Broadphase-accelerated: descend the ray through both BVH trees to
    // a handful of candidate leaves (ray_hits_aabb), then an exact ray-vs-shape test per candidate;
    // the nearest survivor wins. Read-only/const — safe to call between steps, but NOT concurrently
    // with step() (which mutates the bodies and trees the query reads). This is the hitscan /
    // line-of-sight / picking primitive the whole engine reaches for.
    [[nodiscard]] bool raycast(const Ray& ray, RayHit& out, const QueryFilter& filter = {}) const;

    // Collect every body whose shape overlaps the sphere (`center`, `radius`) into `out` (cleared
    // first), in canonical slot order so the result is deterministic run to run. Broadphase-culled,
    // then an exact shape-vs-sphere test. The "what is inside this volume" query — an explosion's
    // affected set (M8), a trigger pre-check. Same const/threading contract as raycast().
    void overlap_sphere(core::Vec3 center,
                        float radius,
                        std::vector<BodyId>& out,
                        const QueryFilter& filter = {}) const;

    // Apply an impulse J (kg·m/s) to a dynamic body at a world-space `point`: Δv = J/m and
    // Δω = I⁻¹(r×J) with r the lever arm from the centre of mass — and WAKE the body (its island
    // reactivates on the next step, like wake_body). A no-op for a stale id or an immovable
    // (static/kinematic, inv_mass == 0) body. This is the gameplay push: projectile hits,
    // explosions (M8), thrusters. `apply_central_impulse` is the through-the-COM shortcut (no
    // angular term).
    void apply_impulse(BodyId id, core::Vec3 impulse, core::Vec3 point) noexcept;
    void apply_central_impulse(BodyId id, core::Vec3 impulse) noexcept;

    // --- Contact & sleep events (M7.9) ------------------------------------------------------
    // The events the most recent step() produced (events.hpp), DOUBLE-BUFFERED: each accessor
    // returns a stable view of the just-completed tick's events that remains valid until the next
    // step(), which refills a back buffer and swaps it in. Both are empty before the first step().
    // Read-only, and NOT safe to call concurrently with step() (which refills the buffers).
    //
    // contact_events(): one entry per contact REGION (a body pair with a dynamic participant,
    // plus — for compounds, M7.12 — the touching child pair within it; one region per pair for
    // plain bodies) that is touching, or just stopped touching, this tick — began/persisted/ended,
    // a representative point, the a→b normal, and the total normal + friction impulse the solver
    // exchanged over the region. This is the M8 damage input (impulse → damage, per part hit). In
    // CANONICAL order (ascending a.index, b.index, then child_a, child_b), so replays and any
    // worker-thread count observe the identical stream.
    [[nodiscard]] std::span<const ContactEvent> contact_events() const noexcept;

    // sleep_events(): one entry per body that fell asleep (Slept — the DebrisSettled basis) or was
    // woken by the simulation (Woke) this tick. In dense body order. See events.hpp on why an
    // explicit wake_body()/apply_impulse() wake is not reported here.
    [[nodiscard]] std::span<const SleepEvent> sleep_events() const noexcept;

    // --- Convex hulls (M7.11, ADR-0027) -----------------------------------------------------
    // Register authored convex-hull geometry with this world and get the id ShapeDesc::hull
    // refers to. Registration is the cold path that pays for everything the hot path needs:
    // validation, face planes, polyhedral mass properties, and the principal-axis
    // decomposition; bodies then instantiate the hull by id (one fracture pattern, many debris
    // bodies — the M8 shape economy). The stored geometry is re-centred on its centre of mass
    // (see HullInfo::centroid). Returns the NULL id (HullId::is_valid() == false) if the input
    // fails validation — see HullDesc for the rules. Hulls are immutable and live as long as
    // the world. Not safe to call concurrently with step() (like create_body).
    [[nodiscard]] HullId register_hull(const HullDesc& desc);

    // Read back a registered hull's derived physical properties. Returns false (out untouched)
    // for a null/unknown id.
    [[nodiscard]] bool hull_info(HullId id, HullInfo& out) const;

    // --- Compound shapes (M7.12, ADR-0028) --------------------------------------------------
    // Register a compound — one rigid body's shape made of several convex children (primitives
    // and/or registered hulls), each at a local pose — and get the id ShapeDesc::compound refers
    // to. The M8 destructible: one intact wall is ONE compound body; on fracture its parts detach
    // into separate hull bodies. Registration is the cold path: validation, the composed mass
    // properties (volume-weighted COM, parallel-axis inertia, principal axes —
    // docs/math/compound-mass-properties.md), and the COM re-centring of the stored child poses
    // (see CompoundInfo::centroid). Returns the NULL id if the input fails validation — see
    // CompoundDesc for the rules. Compounds are immutable and live as long as the world; both
    // static and dynamic compound bodies are supported. Not safe to call concurrently with
    // step() (like create_body / register_hull).
    [[nodiscard]] CompoundId register_compound(const CompoundDesc& desc);

    // Read back a registered compound's derived physical properties. Returns false (out
    // untouched) for a null/unknown id.
    [[nodiscard]] bool compound_info(CompoundId id, CompoundInfo& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::physics
