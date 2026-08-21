// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>

#include "character_fixture.hpp"

// m12.2 proofs: TOUCH versus INSIDE — the distinction the whole controller is built around, and
// the one m12.1 warned about in writing (query.hpp:107-118).
//
// "I touched something after moving 0 m" and "I started inside a wall" are the same number and
// completely different instructions. The first means stop; the second means depenetrate, and a
// controller that treats it as the first freezes solid in the geometry it is stuck in. These cases
// put a character in both situations and assert it behaves differently in each.
//
// The "am I inside anything" probe is Character::overlapping(), which is PhysicsWorld::penetration
// through the public seam — the same measurement the controller's own recovery pass uses, so a
// character that reports itself free really is free by the engine's own definition.
using namespace rime_test;

namespace {

gameplay::CharacterConfig config() {
    return gameplay::CharacterConfig{};
}

} // namespace

TEST_CASE("m12.2 recovery: resting AT a wall still walks at full speed along it") {
    // The touch half. A character standing at its contact offset from a wall is TOUCHING it, and
    // must lose nothing tangentially — this is the case a controller that conflates touch with
    // penetration turns into a stutter.
    physics::PhysicsWorld w;
    (void)add_ground(w);
    (void)add_static_box(w, {0.5f, 6.0f, 8.0f}, {3.5f, 0.0f, 0.0f}); // face at x = +3

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);

    ch.tick_n(walk(1.0f, 0.0f), 60); // push into the wall until it settles against it
    REQUIRE_FALSE(ch.overlapping());
    const float resting_x = ch.state.position.x;

    // Now walk purely ALONG the wall while still leaning on it.
    ch.tick_n(walk(1.0f, 1.0f), 60);

    // The wall costs the into-wall component and NOTHING ELSE. The intent (1, 1) is clamped to the
    // unit disc, so its tangential part is 1/sqrt(2) — and that is exactly what survives.
    const float tangential = -ch.state.velocity.z; // move_y = +1 is world -Z
    const float expected = config().max_speed / std::sqrt(2.0f);
    CHECK(tangential == doctest::Approx(expected).epsilon(0.02));
    CHECK(std::fabs(ch.state.position.x - resting_x) <= ch.config.skin);
    CHECK_FALSE(ch.overlapping());
}

TEST_CASE("m12.2 recovery: a character spawned INSIDE a box digs itself out and stays out") {
    // The inside half, and the m12.1 freeze regression. Nothing here is a nudge: the character is
    // placed 0.1 m inside a solid box and must escape using only the depenetration pass.
    physics::PhysicsWorld w;
    (void)add_ground(w);
    (void)add_static_box(w, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, -4.0f}); // spans z in [-5, -3]

    gameplay::CharacterConfig c = config();
    Character ch;
    // 0.1 m past the near face: the capsule's surface is inside the box by that much.
    ch.spawn(w, c, {0.0f, rest_y(c), -3.0f + c.radius - 0.1f}, /*grounded=*/true);
    REQUIRE(ch.overlapping()); // vacuity: it really did start inside

    // max_depenetration_per_tick is 0.2 m, so one push clears 0.1 m; allow a couple of ticks for
    // the measurement to settle, and no more — the bound is the point.
    const int budget = static_cast<int>(std::ceil(0.1f / c.max_depenetration_per_tick)) + 2;
    int ticks_to_free = -1;
    for (int i = 0; i < budget; ++i) {
        ch.tick(idle());
        if (!ch.overlapping()) {
            ticks_to_free = i + 1;
            break;
        }
    }
    CHECK(ticks_to_free >= 0);
    CHECK(ch.stats.depenetrations > 0); // it got out by RECOVERING, not by drifting
    CHECK(finite(ch.state.position));
    CHECK(finite(ch.state.velocity));

    // And it stays out, with `stuck` never rising again — a stuck that never returns to zero is
    // precisely the freeze this whole mechanism exists to prevent.
    const std::uint32_t stuck_after_escape = ch.stats.stuck;
    ch.tick_n(idle(), 120);
    CHECK(ch.stats.stuck == stuck_after_escape);
    CHECK_FALSE(ch.overlapping());
    CHECK(ch.state.grounded);
}

TEST_CASE("m12.2 recovery: the push is bounded per tick, so a deep overlap slides out visibly") {
    // Buried 1 m deep — five times the per-tick cap. The escape must take several ticks and each
    // one must move at most the cap, because an uncapped depenetration is a teleport.
    physics::PhysicsWorld w;
    (void)add_static_box(w, {4.0f, 4.0f, 4.0f}, {0.0f, 0.0f, 0.0f});

    gameplay::CharacterConfig c = config();
    Character ch;
    ch.spawn(w, c, {0.0f, 4.0f - 1.0f + c.half_height + c.radius, 0.0f});
    REQUIRE(ch.overlapping());

    float max_step = 0.0f;
    int ticks = 0;
    for (; ticks < 200; ++ticks) {
        const core::Vec3 before = ch.state.position;
        ch.tick(idle());
        max_step = std::max(max_step, core::length(ch.state.position - before));
        if (!ch.overlapping()) {
            break;
        }
    }
    CHECK(ticks > 1); // it took SEVERAL ticks: the cap was actually doing something
    CHECK_FALSE(ch.overlapping());
    // Per-tick motion never exceeds the recovery cap plus one tick of ordinary movement (gravity
    // has been acting on it the whole time). The cap is a budget for the TICK, not for each push
    // within it — a tick that needs two pushes still moves at most this far in total.
    CHECK(max_step <= c.max_depenetration_per_tick + c.max_speed * kDt + c.skin);
    CHECK(finite(ch.state.position));
}

TEST_CASE("m12.2 recovery: a zero or non-finite dt advances nothing rather than poisoning state") {
    physics::PhysicsWorld w;
    (void)add_ground(w);

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);
    ch.state.velocity = {1.0f, 0.0f, 2.0f};
    const gameplay::CharacterState before = ch.state;

    for (const float dt : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        const gameplay::CharacterState after =
            gameplay::step_character(before, walk(1.0f, 0.0f), ch.config, w, ch.body, dt, nullptr);
        CHECK(after.position.x == before.position.x);
        CHECK(after.position.y == before.position.y);
        CHECK(after.position.z == before.position.z);
        CHECK(after.velocity.x == before.velocity.x);
        CHECK(after.velocity.z == before.velocity.z);
    }
}

TEST_CASE("m12.2 recovery: a hostile input cannot outrun max_speed") {
    // The mover re-clamps its own input rather than trusting the wire (replication::sanitize runs
    // on arrival, but a replay tape is an input path too). A move vector far outside the unit disc
    // must buy no extra speed at all.
    physics::PhysicsWorld w;
    (void)add_ground(w);

    Character honest;
    honest.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);
    honest.tick_n(walk(1.0f, 0.0f), 60);

    physics::PhysicsWorld w2;
    (void)add_ground(w2);
    Character cheat;
    cheat.spawn(w2, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);
    cheat.tick_n(walk(1000.0f, 1000.0f), 60);

    CHECK(core::length(cheat.state.velocity) <= config().max_speed + 1e-3f);
    CHECK(core::length(cheat.state.velocity) ==
          doctest::Approx(core::length(honest.state.velocity)).epsilon(1e-4));

    // …and a non-finite one is replaced, not propagated.
    gameplay::CharacterInput poison;
    poison.move_x = std::numeric_limits<float>::quiet_NaN();
    poison.yaw = std::numeric_limits<float>::infinity();
    cheat.tick_n(poison, 10);
    CHECK(finite(cheat.state.position));
    CHECK(finite(cheat.state.velocity));
}

TEST_CASE("m12.2 recovery: validate() clamps a broken config instead of trusting it") {
    gameplay::CharacterConfig c;
    c.radius = -1.0f;
    c.skin = 5.0f; // far beyond a quarter of the radius
    c.max_slope_cos = 9.0f;
    c.max_slide_iterations = 0;
    c.gravity = std::numeric_limits<float>::quiet_NaN();

    CHECK(gameplay::validate(c));
    CHECK(c.radius > 0.0f);
    CHECK(c.skin >= 1e-3f);
    CHECK(c.skin <= 0.25f * c.radius);
    CHECK(c.max_slope_cos <= 1.0f);
    CHECK(c.max_slide_iterations >= 1);
    CHECK(std::isfinite(c.gravity));

    // Idempotent: a validated config is already in contract, so nothing moves the second time.
    gameplay::CharacterConfig again = c;
    CHECK_FALSE(gameplay::validate(again));

    // A default config is in contract by construction — a default that needed clamping would mean
    // the documented defaults and the enforced rules had drifted apart.
    gameplay::CharacterConfig defaults;
    CHECK_FALSE(gameplay::validate(defaults));
}
