// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <numbers>

#include "rime/fluids/buoyancy.hpp"
#include "rime/physics/physics.hpp"

using namespace rime;

TEST_CASE("sphere cap volumes have dry, half, and full analytic endpoints") {
    fluids::BuoyancyProbe probe{};
    probe.stable_body_key = 42;
    probe.radius_metres = 0.5f;
    fluids::WaterSurfaceSample sample{};
    sample.authoritative_tick = 17;
    const double full = (4.0 / 3.0) * std::numbers::pi_v<double> * 0.125;

    sample.height_metres = -0.5f;
    const auto dry = fluids::evaluate_buoyancy(sample, probe, 9.81f, 1.0f / 60.0f);
    REQUIRE(dry);
    CHECK(dry->body_key == 42);
    CHECK(dry->water_tick == 17);
    CHECK(dry->displaced_volume_m3 == 0.0f);
    CHECK(dry->body_impulse.y == 0.0f);

    sample.height_metres = 0.0f;
    const auto half = fluids::evaluate_buoyancy(sample, probe, 9.81f, 1.0f / 60.0f);
    REQUIRE(half);
    CHECK(half->displaced_volume_m3 == doctest::Approx(full / 2.0));
    CHECK(half->body_impulse.y == doctest::Approx(1000.0 * 9.81 * full / 120.0));

    sample.height_metres = 0.5f;
    const auto submerged = fluids::evaluate_buoyancy(sample, probe, 9.81f, 1.0f / 60.0f);
    REQUIRE(submerged);
    CHECK(submerged->displaced_volume_m3 == doctest::Approx(full));
    CHECK(submerged->body_impulse.y == doctest::Approx(1000.0 * 9.81 * full / 60.0));
    CHECK(submerged->water_reaction_impulse.y == -submerged->body_impulse.y);
}

TEST_CASE("relative flow drag opposes body motion and exchange reaction is exact") {
    fluids::WaterSurfaceSample sample{};
    sample.height_metres = 1.0f;
    sample.velocity_m_s = {0.0f, 0.0f, 0.0f};
    fluids::BuoyancyProbe probe{};
    probe.radius_metres = 0.5f;
    probe.velocity_world = {2.0f, 0.0f, 0.0f};
    probe.drag_coefficient = 1.0f;
    const auto exchange = fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.01f);
    REQUIRE(exchange);
    CHECK(exchange->body_impulse.x < 0.0f);
    CHECK(exchange->body_impulse.y > 0.0f);
    CHECK(exchange->body_impulse.x + exchange->water_reaction_impulse.x == 0.0f);
    CHECK(exchange->body_impulse.y + exchange->water_reaction_impulse.y == 0.0f);
}

TEST_CASE("reject invalid inputs and outputs that float cannot represent") {
    fluids::WaterSurfaceSample sample{};
    fluids::BuoyancyProbe probe{};
    probe.radius_metres = 1.0f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.0f));
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, -9.81f, 0.01f));
    probe.radius_metres = 0.0f;
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.01f));
    probe.radius_metres = 1.0f;
    probe.drag_coefficient = -1.0f;
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.01f));
    probe.drag_coefficient = 0.0f;
    probe.center_world.x = nan;
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.01f));
    probe.center_world.x = 0.0f;
    sample.velocity_m_s.z = inf;
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.01f));
    sample.velocity_m_s.z = 0.0f;
    sample.density_kg_m3 = 0.0f;
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.01f));
    sample.density_kg_m3 = 1000.0f;
    sample.height_metres = 1.0e20f;
    probe.radius_metres = 1.0e20f;
    CHECK_FALSE(fluids::evaluate_buoyancy(sample, probe, 9.81f, 0.01f));
}

TEST_CASE("public physics impulse holds a half-submerged sphere while dry control falls") {
    constexpr float radius = 0.5f;
    constexpr float dt = 1.0f / 120.0f;
    const float half_volume =
        static_cast<float>((2.0 / 3.0) * std::numbers::pi_v<double> * radius * radius * radius);
    physics::BodyDesc desc{};
    desc.shape = physics::ShapeDesc{physics::ShapeType::Sphere, radius, {}, 0.0f};
    desc.mass = 1000.0f * half_volume;
    physics::PhysicsWorld wet_world;
    physics::PhysicsWorld dry_world;
    wet_world.set_gravity({0.0f, -9.81f, 0.0f});
    dry_world.set_gravity({0.0f, -9.81f, 0.0f});
    const auto wet_id = wet_world.create_body(desc);
    const auto dry_id = dry_world.create_body(desc);
    REQUIRE(wet_id.is_valid());
    REQUIRE(dry_id.is_valid());

    fluids::WaterSurfaceSample surface{};
    surface.height_metres = 0.0f;
    fluids::BuoyancyProbe probe{};
    probe.stable_body_key = 101;
    probe.radius_metres = radius;
    for (std::uint64_t tick = 0; tick < 120; ++tick) {
        physics::BodyState state{};
        REQUIRE(wet_world.get_body_state(wet_id, state));
        probe.center_world = state.position;
        probe.velocity_world = state.linear_velocity;
        surface.authoritative_tick = tick;
        const auto exchange = fluids::evaluate_buoyancy(surface, probe, 9.81f, dt);
        REQUIRE(exchange);
        CHECK(exchange->body_key == 101);
        CHECK(exchange->water_tick == tick);
        wet_world.apply_impulse(wet_id, exchange->body_impulse, exchange->application_point);
        wet_world.step(dt);
        dry_world.step(dt);
    }
    physics::BodyState wet{}, dry{};
    REQUIRE(wet_world.get_body_state(wet_id, wet));
    REQUIRE(dry_world.get_body_state(dry_id, dry));
    CHECK(std::abs(wet.position.y) < 0.03f);
    CHECK(std::abs(wet.linear_velocity.y) < 0.06f);
    CHECK(dry.position.y < -4.0f);
}
