// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp"

// m12.1 proofs: KINEMATIC PUSH-IN — the direction PhysicsSync did not have. Write-back is
// "the simulation moved it, tell the game"; push-in is "the game moved it, tell the simulation",
// and it is what lets a character controller occupy a kinematic capsule that debris and crates
// actually collide with (ADR-0035 §3).
//
// Three things here are worth more than the plumbing they look like:
//
//   * THE VELOCITY, not just the pose. A teleport arrives overlapping whatever it moved into and
//     the solver reads that as penetration — which fires the crate off. Handing the solver the
//     velocity the move implies makes the same contact resolve as a PUSH at walking speed.
//   * NO DOUBLE-MOVE. Setting a velocity on a body the solver also integrated would move it twice.
//     Kinematic bodies are island ANCHORS rather than members, so they are not integrated — and
//     that is asserted here rather than assumed, because it is exactly the kind of invariant a
//     later solver change could break silently.
//   * THE SKIP PATH HAS AN EXIT. "Unmoved, so do nothing" is correct only until the game STOPS
//     moving the body: without a final zeroing push, the solver keeps resolving contacts against a
//     capsule it believes is still walking, and the crate slides away on its own forever.
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

ecs::Entity spawn_body(ecs::World& w,
                       core::Vec3 pos,
                       physics::MotionType motion,
                       core::Vec3 half,
                       float mass = 1.0f) {
    physics::RigidBody rb;
    rb.motion = static_cast<std::uint32_t>(motion);
    rb.mass = mass;
    physics::Collider col;
    col.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Box);
    col.half_x = half.x;
    col.half_y = half.y;
    col.half_z = half.z;
    ecs::WorldTransform wt;
    wt.value.translation = pos;
    return w.spawn_with(rb, col, wt);
}

// Move an entity's WorldTransform the way a game system would: straight through the component,
// WITHOUT marking it changed. That is deliberate — push-in must not depend on the change flag,
// because a game is free to write here and never mark, and a skip path with a blind spot is one
// that silently stops working.
void move_to(ecs::World& w, ecs::Entity e, core::Vec3 pos) {
    w.get<ecs::WorldTransform>(e)->value.translation = pos;
}

physics::BodyState state_of(ecs::World& w, physics::PhysicsWorld& pw, ecs::Entity e) {
    physics::BodyState s;
    const physics::RigidBodyHandle* h = w.get<physics::RigidBodyHandle>(e);
    REQUIRE(h != nullptr);
    REQUIRE(pw.get_body_state(h->body, s));
    return s;
}

} // namespace

TEST_CASE("m12.1 push-in: a kinematic body follows the pose the game wrote") {
    ecs::World w;
    physics::register_physics_components(w);
    physics::PhysicsWorld pw;
    physics::PhysicsSync sync;

    const ecs::Entity e =
        spawn_body(w, {0.0f, 0.0f, 0.0f}, physics::MotionType::Kinematic, {0.5f, 0.5f, 0.5f});
    sync.step(w, pw, kDt); // binds at the spawn pose

    move_to(w, e, {1.0f, 0.0f, 0.0f});
    sync.step(w, pw, kDt);

    const physics::BodyState s = state_of(w, pw, e);
    SUBCASE("the body is where the game put it — and NOT one integration step past it") {
        // The trap this asserts against: push-in sets both the pose AND a velocity. If kinematic
        // bodies were integrated like dynamic ones, the body would end this tick at
        // 1.0 + 60.0 * dt = 2.0. They are island anchors, so it does not.
        CHECK(s.position.x == doctest::Approx(1.0f));
        CHECK(s.position.y == doctest::Approx(0.0f));
    }

    SUBCASE("the velocity is the one the move implies") {
        // 1.0 m in one 1/60 s tick = 60 m/s. That number is what the solver needs in order to
        // resolve the contact as a push rather than as a penetration.
        CHECK(s.linear_velocity.x == doctest::Approx(60.0f).epsilon(0.001));
    }
}

TEST_CASE("m12.1 push-in: a rotation gives an angular velocity, the short way round") {
    ecs::World w;
    physics::register_physics_components(w);
    physics::PhysicsWorld pw;
    physics::PhysicsSync sync;

    const ecs::Entity e =
        spawn_body(w, {0.0f, 0.0f, 0.0f}, physics::MotionType::Kinematic, {0.5f, 0.5f, 0.5f});
    sync.step(w, pw, kDt);

    // A small yaw. ω ≈ angle/dt about +Y.
    constexpr float kAngle = 0.05f;
    w.get<ecs::WorldTransform>(e)->value.rotation =
        core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, kAngle);
    sync.step(w, pw, kDt);

    const physics::BodyState s = state_of(w, pw, e);
    CHECK(s.angular_velocity.y == doctest::Approx(kAngle / kDt).epsilon(0.01));
    CHECK(std::fabs(s.angular_velocity.x) < 1e-3f);
    CHECK(std::fabs(s.angular_velocity.z) < 1e-3f);

    SUBCASE("a rotation past half a turn takes the SHORT way, not the long one") {
        // q and −q are the same rotation but their differences are not. Without the double-cover
        // flip, a delta whose w came out negative reports an angular velocity pointing backwards
        // at an enormous magnitude — the classic symptom being a door that snaps the wrong way.
        w.get<ecs::WorldTransform>(e)->value.rotation =
            core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, kAngle + 0.05f);
        sync.step(w, pw, kDt);
        const physics::BodyState s2 = state_of(w, pw, e);
        CHECK(s2.angular_velocity.y > 0.0f); // same direction…
        CHECK(s2.angular_velocity.y == doctest::Approx(0.05f / kDt).epsilon(0.02)); // …same rate
    }
}

TEST_CASE("m12.1 push-in: the skip path has an exit — a body the game stops moving comes to rest") {
    ecs::World w;
    physics::register_physics_components(w);
    physics::PhysicsWorld pw;
    physics::PhysicsSync sync;

    const ecs::Entity e =
        spawn_body(w, {0.0f, 0.0f, 0.0f}, physics::MotionType::Kinematic, {0.5f, 0.5f, 0.5f});
    sync.step(w, pw, kDt);

    move_to(w, e, {0.1f, 0.0f, 0.0f});
    sync.step(w, pw, kDt);
    REQUIRE(state_of(w, pw, e).linear_velocity.x > 1.0f); // it is moving

    // The game stops writing. Without the final zeroing push the body would keep the velocity of
    // its last move forever, and anything it was touching would keep being pushed.
    sync.step(w, pw, kDt);
    const physics::BodyState resting = state_of(w, pw, e);
    CHECK(resting.linear_velocity.x == doctest::Approx(0.0f));
    CHECK(resting.position.x == doctest::Approx(0.1f)); // and it stayed where it was put

    SUBCASE("and it stays at rest for as long as the game leaves it alone") {
        for (int i = 0; i < 10; ++i) {
            sync.step(w, pw, kDt);
        }
        const physics::BodyState still = state_of(w, pw, e);
        CHECK(still.linear_velocity.x == doctest::Approx(0.0f));
        CHECK(still.position.x == doctest::Approx(0.1f));
    }
}

TEST_CASE("m12.1 push-in: a DYNAMIC body is not pushed in — the simulation owns its pose") {
    ecs::World w;
    physics::register_physics_components(w);
    physics::PhysicsWorld pw;
    physics::PhysicsSync sync;
    pw.set_gravity({0.0f, 0.0f, 0.0f}); // isolate the question from falling

    const ecs::Entity e =
        spawn_body(w, {0.0f, 0.0f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
    sync.step(w, pw, kDt);

    // A game that writes a dynamic entity's WorldTransform is fighting the solver; push-in must
    // not help it. (Teleporting a dynamic body is what PhysicsWorld::set_body_state is for, and it
    // is a deliberate act.)
    move_to(w, e, {5.0f, 0.0f, 0.0f});
    sync.step(w, pw, kDt);

    CHECK(state_of(w, pw, e).position.x == doctest::Approx(0.0f));
}

TEST_CASE("m12.1 push-in: a moving kinematic body PUSHES a dynamic one") {
    // The gameplay-level statement the whole feature exists for: walk a kinematic capsule into a
    // crate and the crate moves — at a plausible speed, in the direction of travel, rather than
    // being fired off by a penetration the solver had to undo.
    ecs::World w;
    physics::register_physics_components(w);
    physics::PhysicsWorld pw;
    physics::PhysicsSync sync;
    pw.set_gravity({0.0f, 0.0f, 0.0f});

    const ecs::Entity pusher =
        spawn_body(w, {-2.0f, 0.0f, 0.0f}, physics::MotionType::Kinematic, {0.5f, 0.5f, 0.5f});
    const ecs::Entity crate =
        spawn_body(w, {0.0f, 0.0f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
    sync.step(w, pw, kDt);

    // Walk it in at ~3 m/s for a second of ticks.
    core::Vec3 at{-2.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60; ++i) {
        at.x += 3.0f * kDt;
        move_to(w, pusher, at);
        sync.step(w, pw, kDt);
    }

    const physics::BodyState crate_state = state_of(w, pw, crate);
    CHECK(crate_state.position.x > 0.2f);        // it was shoved along…
    CHECK(crate_state.position.x < 3.0f);        // …but not launched
    CHECK(crate_state.linear_velocity.x > 0.0f); // and it is moving the way the pusher went

    SUBCASE("the pusher itself is exactly where the game put it") {
        // Nothing the solver did moved the kinematic body: it is an anchor, and the game's pose is
        // the whole truth about where it is.
        CHECK(state_of(w, pw, pusher).position.x == doctest::Approx(at.x));
    }
}

TEST_CASE("m12.1 push-in: an unmoved body is not re-pushed, and write-back leaves it alone") {
    ecs::World w;
    physics::register_physics_components(w);
    physics::PhysicsWorld pw;
    physics::PhysicsSync sync;

    const ecs::Entity e =
        spawn_body(w, {3.0f, 4.0f, 5.0f}, physics::MotionType::Kinematic, {0.5f, 0.5f, 0.5f});

    for (int i = 0; i < 5; ++i) {
        sync.step(w, pw, kDt);
    }

    // Write-back only ever touches DYNAMIC bodies, so a kinematic entity's transform stays exactly
    // as the game left it — no drift, no round-trip through the solver.
    const ecs::WorldTransform* wt = w.get<ecs::WorldTransform>(e);
    REQUIRE(wt != nullptr);
    CHECK(wt->value.translation.x == 3.0f);
    CHECK(wt->value.translation.y == 4.0f);
    CHECK(wt->value.translation.z == 5.0f);
    CHECK(state_of(w, pw, e).linear_velocity.x == doctest::Approx(0.0f));
}
