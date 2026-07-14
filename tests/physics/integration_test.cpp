// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "rime/core/hash.hpp"
#include "rime/physics/physics.hpp"

// M7.1 proofs: the body pool is churn-safe (generational ids) and the integrator is correct
// (ballistic trajectories match the closed form) and deterministic (same inputs ⇒ identical bytes).
// All pure-CPU — no GPU, no collision (that starts M7.2).
using namespace rime;

namespace {

// Fold every body's motion state into one FNV-1a hash, in id order — a faithful witness of the
// simulated state (the method M7.5 reuses to prove thread-count independence, and M11 replays).
// Hash the float VALUES, not the raw BodyState bytes: BodyState carries alignment padding (Quat is
// alignas(16), so the struct is 64 bytes for 52 of data), and those padding bytes are indeterminate
// — `BodyState{}` does NOT reliably zero them on every compiler (Apple-clang/ARM64 leaves stack
// garbage), so hashing the raw struct makes the digest depend on the stack and differ run to run.
// Packing the floats is exactly what PhysicsWorld::world_hash does, and for the same reason.
std::uint64_t hash_states(const physics::PhysicsWorld& world,
                          const std::vector<physics::BodyId>& ids) {
    std::uint64_t h = core::kFnv1a64OffsetBasis;
    for (const physics::BodyId id : ids) {
        physics::BodyState s{};
        (void)world.get_body_state(id, s);
        const std::array<float, 13> v = {s.position.x,
                                         s.position.y,
                                         s.position.z,
                                         s.orientation.x,
                                         s.orientation.y,
                                         s.orientation.z,
                                         s.orientation.w,
                                         s.linear_velocity.x,
                                         s.linear_velocity.y,
                                         s.linear_velocity.z,
                                         s.angular_velocity.x,
                                         s.angular_velocity.y,
                                         s.angular_velocity.z};
        h = core::fnv1a_64(std::as_bytes(std::span<const float>{v}), h);
    }
    return h;
}

} // namespace

TEST_CASE("free fall matches the semi-implicit-Euler closed form") {
    physics::PhysicsWorld world;
    world.set_gravity({0.0f, -9.81f, 0.0f});

    physics::BodyDesc d;
    d.motion = physics::MotionType::Dynamic;
    d.shape = physics::ShapeDesc{physics::ShapeType::Sphere, 0.5f, {}, 0.0f};
    d.mass = 1.0f;
    const physics::BodyId id = world.create_body(d);

    const float dt = 1.0f / 240.0f;
    const int steps = 240; // one second
    for (int k = 0; k < steps; ++k) {
        world.step(dt);
    }

    physics::BodyState s;
    REQUIRE(world.get_body_state(id, s));

    const float g = 9.81f;
    const float t = static_cast<float>(steps) * dt;

    // Velocity is EXACT under semi-implicit Euler for constant acceleration: v = -g·t.
    CHECK(s.linear_velocity.y == doctest::Approx(-g * t).epsilon(0.0005));

    // Position matches the discrete closed form y = -g·dt²·N(N+1)/2 (the sum of the per-step
    // drops).
    const float y_closed = -g * dt * dt * (static_cast<float>(steps) * (steps + 1) / 2.0f);
    CHECK(s.position.y == doctest::Approx(y_closed).epsilon(0.0005));

    // And within the known O(dt) bias of the analytic ½·g·t² (≈2 cm low at t=1 s, dt=1/240).
    CHECK(s.position.y == doctest::Approx(-0.5f * g * t * t).epsilon(0.01));
}

TEST_CASE("generational ids detect use-after-free across pool reuse") {
    physics::PhysicsWorld world;
    physics::BodyDesc d;
    d.shape = physics::ShapeDesc{physics::ShapeType::Box, 0.0f, {0.5f, 0.5f, 0.5f}, 0.0f};

    const physics::BodyId a = world.create_body(d);
    CHECK(world.is_alive(a));
    CHECK(world.body_count() == 1);

    world.destroy_body(a);
    CHECK_FALSE(world.is_alive(a));
    CHECK(world.body_count() == 0);

    const physics::BodyId b = world.create_body(d); // reuses a's slot with a bumped generation
    CHECK(world.is_alive(b));
    CHECK(a != b);

    physics::BodyState s;
    CHECK_FALSE(world.get_body_state(a, s)); // the stale handle reads as dead, not as b
    CHECK(world.get_body_state(b, s));
}

TEST_CASE("swap-remove keeps other bodies' ids valid under churn") {
    physics::PhysicsWorld world;
    world.set_gravity({0.0f, 0.0f, 0.0f});
    physics::BodyDesc d;

    std::vector<physics::BodyId> ids;
    for (int i = 0; i < 8; ++i) {
        d.position = {static_cast<float>(i), 0.0f, 0.0f};
        ids.push_back(world.create_body(d));
    }
    // Destroy a middle body — its dense slot gets back-filled by the last body via swap-remove.
    world.destroy_body(ids[3]);
    CHECK(world.body_count() == 7);
    CHECK_FALSE(world.is_alive(ids[3]));

    // Every surviving id still resolves to its own body at the right position.
    for (int i = 0; i < 8; ++i) {
        if (i == 3) {
            continue;
        }
        physics::BodyState s;
        REQUIRE(world.get_body_state(ids[i], s));
        CHECK(s.position.x == doctest::Approx(static_cast<float>(i)));
    }
}

TEST_CASE("static and kinematic bodies ignore gravity") {
    physics::PhysicsWorld world;
    world.set_gravity({0.0f, -9.81f, 0.0f});

    physics::BodyDesc st;
    st.motion = physics::MotionType::Static;
    st.position = {0.0f, 10.0f, 0.0f};
    const physics::BodyId s_id = world.create_body(st);

    physics::BodyDesc kin;
    kin.motion = physics::MotionType::Kinematic;
    kin.position = {5.0f, 10.0f, 0.0f};
    const physics::BodyId k_id = world.create_body(kin);

    for (int k = 0; k < 120; ++k) {
        world.step(1.0f / 60.0f);
    }

    physics::BodyState s;
    REQUIRE(world.get_body_state(s_id, s));
    CHECK(s.position.y == doctest::Approx(10.0f));
    REQUIRE(world.get_body_state(k_id, s));
    CHECK(s.position.y == doctest::Approx(10.0f));
}

TEST_CASE("stepping is deterministic run-to-run (same-binary)") {
    const auto run = [] {
        physics::PhysicsWorld world;
        world.set_gravity({0.0f, -9.81f, 0.0f});
        std::vector<physics::BodyId> ids;
        for (int i = 0; i < 16; ++i) {
            physics::BodyDesc d;
            d.shape = physics::ShapeDesc{physics::ShapeType::Sphere, 0.5f, {}, 0.0f};
            d.position = {static_cast<float>(i), 5.0f, static_cast<float>(-i)};
            d.linear_velocity = {static_cast<float>(i) * 0.1f, 0.0f, 1.0f};
            d.angular_velocity = {0.0f, static_cast<float>(i) * 0.2f, 0.0f};
            d.angular_damping = 0.01f;
            ids.push_back(world.create_body(d));
        }
        for (int k = 0; k < 300; ++k) {
            world.step(1.0f / 120.0f);
        }
        return hash_states(world, ids);
    };
    CHECK(run() == run());
}
