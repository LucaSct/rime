// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "rime/core/math/vec.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/contact.hpp"

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
    // sphere/capsule pairs, GJK + EPA + reference-face clipping for boxes (and convex hulls at
    // M7.9). Each manifold carries feature-id-tagged points whose accumulated impulses persist
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::physics
