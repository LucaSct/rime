// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <vector>

#include "rime/ecs/entity.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/world.hpp"

// PhysicsSync — the bridge that keeps an ecs::World and a PhysicsWorld in step (M7.6). It is the
// seam ADR-0026 left between "the game's data" (entities with RigidBody/Collider/WorldTransform
// components) and "the simulation" (a PhysicsWorld of BodyIds), and it does three jobs across a
// fixed tick:
//
//   * BIND — an entity that carries the intent components but has no body yet gets one created from
//     them (placed at its WorldTransform), and a RigidBodyHandle component linking the two.
//   * WRITE-BACK — after the step, each AWAKE dynamic body's pose is written back into its entity's
//     WorldTransform, and that component is stamped changed at the world's current version. This is
//     where M7.5 sleeping and M7.6 change detection pay off together: a settled world writes back
//     nothing and stamps nothing, so a change-tracking consumer (GPU upload, editor sync, M11
//     replication) visits only the handful of bodies that actually moved.
//   * UNBIND — a body whose entity was despawned (or dropped its RigidBody) is destroyed. Because a
//     despawn takes the RigidBodyHandle with it, a query could never find the orphan; so the bridge
//     owns an entity↔body ROSTER as the authoritative cleanup list.
//
// Kinematic push-in (game moves a WorldTransform → drive the body) and physics→LocalTransform sync
// for parented bodies are deferred (v1 treats a simulated body's WorldTransform as physics-owned);
// the seam is here to grow into. The canonical tick order is written up in
// docs/design/simulation-tick.md.
namespace rime::ecs {
class World;
}

namespace rime::physics {

class PhysicsSync {
public:
    PhysicsSync() = default;

    // Create bodies for newly-intent entities (RigidBody + Collider + WorldTransform, no handle
    // yet) and destroy bodies whose entity died or dropped its RigidBody/Collider. Run at the top
    // of the fixed tick, before step(). Structural: it adds/removes RigidBodyHandle components, so
    // it must NOT run concurrently with systems iterating those archetypes (call it between phases
    // / on the main thread, like any structural change).
    void reconcile(ecs::World& world, PhysicsWorld& physics);

    // Write every AWAKE dynamic body's pose back into its entity's WorldTransform and stamp that
    // component changed at the world's current version (World::mark_changed). Run after step(). The
    // caller advances the world change version once per tick (World::advance_version or
    // Schedule::run) BEFORE the tick's writes, so these stamps land at a version a consumer's
    // pre-tick checkpoint can detect.
    void write_back(ecs::World& world, PhysicsWorld& physics);

    // Convenience for the common fixed-tick body: reconcile → physics.step(dt) → write_back.
    void step(ecs::World& world, PhysicsWorld& physics, float dt);

    // How many entity↔body bindings the bridge currently tracks.
    [[nodiscard]] std::size_t bound_count() const noexcept { return bound_.size(); }

private:
    struct Bound {
        ecs::Entity entity;
        BodyId body;
        bool
            dynamic; // only dynamic bodies are written back (static/kinematic never move under sim)
    };

    std::vector<Bound> bound_;
};

} // namespace rime::physics
