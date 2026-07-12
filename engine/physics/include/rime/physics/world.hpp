// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <memory>

#include "rime/core/math/vec.hpp"
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rime::physics
