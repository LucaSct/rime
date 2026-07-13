// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "rime/core/math/vec.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/body.hpp"

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

    // Advance the simulation by a fixed timestep. M7.1 integrates forces → velocities → positions
    // (semi-implicit Euler) with no collision; broadphase/narrowphase/solve land in M7.2–M7.4.
    // Called from inside the app's fixed tick (ADR-0023), never per render frame.
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::physics
