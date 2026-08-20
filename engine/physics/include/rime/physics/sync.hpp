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
//   * PUSH-IN (m12.1) — the other direction, and the one the character controller needs: a game
//     that moves a KINEMATIC body's WorldTransform has the physics body driven to match, with the
//     velocity implied by the move, so the thing it walks into is pushed rather than teleported
//     through. Runs between reconcile and step.
//
// physics→LocalTransform sync for PARENTED bodies is still deferred (v1 binds root bodies only);
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

    // Drive every bound KINEMATIC body from its entity's WorldTransform: if the game moved the
    // entity since the last push, teleport the body there AND give it the velocity that move
    // implies over `dt`. Run after reconcile() and BEFORE step(). No-op for dynamic bodies (the
    // simulation owns their pose — a game writing it would be fighting the solver) and for static
    // ones (which do not move by definition).
    //
    // The velocity is the point, not a bonus. `set_body_state` alone teleports: a capsule moved
    // into a crate would arrive overlapping it and the solver would resolve that as a penetration,
    // firing the crate off. Handing the solver `(new - old) / dt` instead means the contact is
    // solved with the true relative motion, so the crate is PUSHED at walking speed. Kinematic
    // bodies are anchors rather than island members, so their pose is never integrated from that
    // velocity — setting it cannot double-move them.
    //
    // "Did it move?" is answered by comparing against the pose this bridge last pushed, not by the
    // ECS change flag. A game can legitimately write through `world.get<WorldTransform>()` without
    // calling mark_changed, so the flag has a blind spot; the stored pose has none, and costs one
    // vector-and-quaternion compare per kinematic body per tick.
    void push_in(ecs::World& world, PhysicsWorld& physics, float dt);

    // Write every AWAKE dynamic body's pose back into its entity's WorldTransform and stamp that
    // component changed at the world's current version (World::mark_changed). Run after step(). The
    // caller advances the world change version once per tick (World::advance_version or
    // Schedule::run) BEFORE the tick's writes, so these stamps land at a version a consumer's
    // pre-tick checkpoint can detect.
    void write_back(ecs::World& world, PhysicsWorld& physics);

    // Convenience for the common fixed-tick body:
    // reconcile → push_in → physics.step(dt) → write_back.
    void step(ecs::World& world, PhysicsWorld& physics, float dt);

    // How many entity↔body bindings the bridge currently tracks.
    [[nodiscard]] std::size_t bound_count() const noexcept { return bound_.size(); }

private:
    struct Bound {
        ecs::Entity entity;
        BodyId body;
        // The motion class decides which direction this binding syncs: DYNAMIC is written back
        // (the sim owns the pose), KINEMATIC is pushed in (the game owns it), STATIC is neither.
        MotionType motion = MotionType::Dynamic;
        // The pose push_in last drove this body to — the reference "did the game move it?" is
        // measured against. Meaningful for kinematic bindings; seeded at bind time from the
        // spawn pose so a freshly-bound body neither pushes nor invents a velocity on its first
        // tick.
        core::Vec3 pushed_position{0.0f, 0.0f, 0.0f};
        core::Quat pushed_orientation = core::quat_identity();
        // Whether the last push carried a non-zero velocity. This is what gives the "unmoved, so
        // skip" path an EXIT: a body the game stops moving needs one final push that zeroes its
        // velocity, or the solver keeps resolving contacts against a capsule it believes is still
        // walking, and the crate it was pushing slides away on its own forever.
        bool pushed_moving = false;
    };

    std::vector<Bound> bound_;
};

} // namespace rime::physics
