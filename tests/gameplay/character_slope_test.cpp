// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN // this TU supplies doctest's main() for the exe
#include <doctest/doctest.h>

#include <cmath>

#include "character_fixture.hpp"

// m12.2 proofs: SLOPES — hold, climb, and slide off.
//
// The slope limit is the controller's single most visible rule, and all three of its outcomes have
// closed forms. The climb speed in particular is not a tuned number: the controller projects its
// wish velocity onto the supporting plane, so walking straight up an incline of θ must produce an
// along-slope speed of exactly max_speed * cos θ. That is the arithmetic these cases check.
using namespace rime_test;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

gameplay::CharacterConfig config() {
    gameplay::CharacterConfig c;
    c.max_slope_cos = std::cos(45.0f * kDeg); // the 45° limit these cases are written around
    return c;
}

} // namespace

TEST_CASE("m12.2 slope: a 30 degree incline is climbed at max_speed * cos(slope)") {
    const float theta = 30.0f * kDeg;
    physics::PhysicsWorld w;
    add_ramp(w, theta, {60.0f, 0.5f, 20.0f});

    Character ch;
    ch.spawn(w, config(), rest_on_slope(config(), theta, -20.0f, 0.0f), /*grounded=*/true);
    ch.state.ground_normal = slope_normal(theta);

    const core::Vec3 start = ch.state.position;
    constexpr int kTicks = 300;
    ch.tick_n(walk(1.0f, 0.0f), kTicks); // +move_x is world +X, which is uphill here

    // Vacuity guards (m11.7's discipline): a test that never cast and never touched the ground
    // would pass every assertion below while proving nothing. (No `snaps` guard here on purpose —
    // climbing means velocity.y > 0, and the snap deliberately does not run then. The hold case
    // below is where the snap is exercised.)
    CHECK(ch.stats.casts > 0);
    CHECK(ch.state.grounded);

    const core::Vec3 moved = ch.state.position - start;
    const float travelled = core::length(moved);
    const float expected = config().max_speed * std::cos(theta) * (kTicks * kDt);

    // ±5%, and generous by two orders of magnitude on purpose. The real per-tick error is the
    // query residual (~2.3e-4 m) plus at most one skin-sized repositioning per contact change —
    // millimetres against twenty-six metres. The margin is for the acceleration ramp at the start
    // (~0.1 s to reach speed), which is a real 1% of a 5-second walk.
    CHECK(travelled == doctest::Approx(expected).epsilon(0.05));

    // …and it went UP the slope, not along the flat or into it.
    const core::Vec3 uphill = slope_uphill(theta);
    CHECK(core::dot(moved, uphill) == doctest::Approx(travelled).epsilon(0.01));
    CHECK(ch.state.position.y > start.y + 10.0f);
    CHECK_FALSE(ch.overlapping());
}

TEST_CASE("m12.2 slope: a 30 degree incline holds a still character without creep") {
    const float theta = 30.0f * kDeg;
    physics::PhysicsWorld w;
    add_ramp(w, theta, {60.0f, 0.5f, 20.0f});

    Character ch;
    ch.spawn(w, config(), rest_on_slope(config(), theta, 0.0f, 0.0f), /*grounded=*/true);
    ch.state.ground_normal = slope_normal(theta);

    ch.tick_n(idle(), 30); // settle: the snap converges on its resting clearance in one tick
    const core::Vec3 settled = ch.state.position;

    ch.tick_n(idle(), 300);
    CHECK(ch.state.grounded);
    CHECK(ch.stats.casts > 0);
    CHECK(ch.stats.snaps > 0); // standing still is exactly when the ground snap does its work
    // A walkable slope holds you. Any per-tick residual would show up multiplied by 300 here,
    // which is exactly why the window is 300 ticks and the bound is one skin.
    CHECK(core::length(ch.state.position - settled) <= ch.config.skin);
}

TEST_CASE("m12.2 slope: a 60 degree face refuses to ground and slides a character down it") {
    const float theta = 60.0f * kDeg;
    physics::PhysicsWorld w;
    add_ramp(w, theta, {60.0f, 0.5f, 20.0f});

    Character ch;
    ch.spawn(w, config(), rest_on_slope(config(), theta, 0.0f, 0.0f));

    const core::Vec3 start = ch.state.position;
    ch.tick_n(walk(1.0f, 0.0f), 120); // pushing UPHILL the whole time

    const core::Vec3 uphill = slope_uphill(theta);
    const core::Vec3 moved = ch.state.position - start;

    // Two assertions, and the second is the one that matters. "No uphill progress" alone would
    // also pass for a controller that simply STUCK to the steep face — which is the classic bug
    // this case exists to catch — so the character must be measurably sliding down as well.
    CHECK(core::dot(moved, uphill) <= 0.0f);
    CHECK(core::dot(ch.state.velocity, uphill) < -0.5f);
    CHECK_FALSE(ch.state.grounded); // too steep to stand on, whatever it is touching
    CHECK_FALSE(ch.overlapping());
    CHECK(finite(ch.state.position));
    CHECK(finite(ch.state.velocity));
}

TEST_CASE("m12.2 slope: the walkable limit is the configured cosine, not a hard-coded one") {
    // Same geometry, two configs. A 40° ramp is walkable under a 45° limit and not under a 30°
    // one — which proves the rule is read from the config rather than baked into the algorithm.
    const float theta = 40.0f * kDeg;

    gameplay::CharacterConfig permissive = config();
    gameplay::CharacterConfig strict = config();
    strict.max_slope_cos = std::cos(30.0f * kDeg);

    physics::PhysicsWorld w1;
    add_ramp(w1, theta, {40.0f, 0.5f, 20.0f});
    Character loose;
    loose.spawn(w1, permissive, rest_on_slope(permissive, theta, 0.0f, 0.0f), /*grounded=*/true);
    loose.state.ground_normal = slope_normal(theta);
    loose.tick_n(idle(), 60);
    CHECK(loose.state.grounded);

    physics::PhysicsWorld w2;
    add_ramp(w2, theta, {40.0f, 0.5f, 20.0f});
    Character tight;
    tight.spawn(w2, strict, rest_on_slope(strict, theta, 0.0f, 0.0f), /*grounded=*/true);
    tight.state.ground_normal = slope_normal(theta);
    tight.tick_n(idle(), 60);
    CHECK_FALSE(tight.state.grounded);
}
