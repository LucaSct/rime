// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>

#include "character_fixture.hpp"

// m12.2 proofs: STAIRS — what gets climbed, what gets refused, and what stays grounded on the way
// down.
//
// `step_height` is a promise with two sides, and the second one is the one that gets forgotten: a
// controller that climbs a 0.25 m step under a 0.3 m budget is only half correct if it also climbs
// a 0.40 m one. Both bounds are asserted here, along with the step-DOWN behaviour that keeps a
// character walking down a staircase instead of performing a series of small falls.
using namespace rime_test;

namespace {

gameplay::CharacterConfig config() {
    gameplay::CharacterConfig c;
    c.step_height = 0.3f;
    c.snap_distance = 0.3f;
    return c;
}

// A step: a static box whose TOP surface is at `height`, whose front FACE is at z = -3, extending
// away from the character. Wide enough that the capsule never reaches a side edge.
constexpr float kStepFaceZ = -3.0f;

void add_step(physics::PhysicsWorld& w, float height) {
    // 16 m of tread beyond the riser, so a character that climbs it has somewhere to keep walking
    // for the rest of the case and never reaches a far edge.
    (void)add_static_box(w, {8.0f, height * 0.5f, 8.0f}, {0.0f, height * 0.5f, kStepFaceZ - 8.0f});
}

} // namespace

TEST_CASE("m12.2 step: a riser inside step_height is climbed") {
    physics::PhysicsWorld w;
    (void)add_ground(w);
    add_step(w, 0.25f); // well inside the 0.3 m budget

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);

    const float start_y = ch.state.position.y;
    ch.tick_n(walk(0.0f, 1.0f), 120); // +move_y is world -Z: straight at the step

    CHECK(ch.stats.casts > 0);
    CHECK(ch.stats.steps_climbed > 0); // vacuity: the step-up ladder actually ran
    CHECK(ch.state.grounded);
    // Standing on the 0.25 m tread now, within the contact offset plus a query residual.
    CHECK(std::fabs(ch.state.position.y - (start_y + 0.25f)) <= ch.config.skin + 1e-3f);
    CHECK(ch.state.position.z < kStepFaceZ); // …and it got PAST the riser, not merely up
    CHECK_FALSE(ch.overlapping());
}

TEST_CASE("m12.2 step: a riser above step_height is refused, and refused visibly") {
    physics::PhysicsWorld w;
    (void)add_ground(w);
    add_step(w, 0.40f); // beyond the 0.3 m budget

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);

    const float start_y = ch.state.position.y;
    ch.tick_n(walk(0.0f, 1.0f), 120);

    CHECK(ch.stats.steps_climbed == 0);
    CHECK(ch.stats.step_rejected > 0); // the refusal is COUNTED, not silent
    CHECK(ch.state.position.y == doctest::Approx(start_y).epsilon(0.05));
    CHECK(ch.state.position.z > kStepFaceZ); // stopped at the face, not through it
    CHECK(ch.state.grounded);
    CHECK_FALSE(ch.overlapping());
}

TEST_CASE("m12.2 step: walking off a small ledge stays grounded via the snap") {
    // A 0.25 m drop is inside snap_distance, so the character walks down it rather than falling.
    // This is the reason a staircase does not feel like a series of small falls.
    physics::PhysicsWorld w;
    // Upper floor's top is y = 0 and its far edge z = -6; the lower floor abuts it there, with its
    // top 0.25 m down. No gap between them — a gap would be a pit, not a step.
    (void)add_static_box(w, {8.0f, 0.5f, 4.0f}, {0.0f, -0.5f, -2.0f});
    (void)add_static_box(w, {8.0f, 0.5f, 6.0f}, {0.0f, -0.75f, -12.0f});

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);
    ch.tick_n(walk(0.0f, 1.0f), 90); // ~9 m: past the edge at z = -6, short of the far end

    CHECK(ch.state.position.z < -7.0f); // it is out over the lower floor
    CHECK(ch.stats.snaps > 0);
    CHECK(ch.state.grounded); // never left the ground
    CHECK(ch.state.position.y == doctest::Approx(rest_y(config(), -0.25f)).epsilon(0.05));
    CHECK_FALSE(ch.overlapping());
}

TEST_CASE("m12.2 step: walking off a tall ledge goes airborne and falls") {
    // A 3 m drop is far beyond snap_distance: the snap must NOT reach for it, or a character
    // would glue itself to any floor it ever walked near.
    physics::PhysicsWorld w;
    (void)add_static_box(w, {8.0f, 0.5f, 4.0f}, {0.0f, -0.5f, -2.0f});  // top y = 0, edge z = -6
    (void)add_static_box(w, {8.0f, 0.5f, 8.0f}, {0.0f, -3.5f, -14.0f}); // top y = -3

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);

    // Walk to the edge and well past it — the edge is 6 m away at 6 m/s, so ~60 ticks, and the
    // extra 30 are what make "is it falling" a question with an answer.
    int airborne_ticks = 0;
    for (int i = 0; i < 90; ++i) {
        ch.tick(walk(0.0f, 1.0f));
        if (!ch.state.grounded) {
            ++airborne_ticks;
        }
    }
    CHECK(airborne_ticks > 0);
    CHECK_FALSE(ch.state.grounded);
    CHECK(ch.state.position.y < rest_y(config()) - 0.1f); // genuinely falling, not hovering
    CHECK(ch.state.velocity.y < 0.0f);

    // …and it lands, on the lower floor, and stays there. Input released, so the landing is
    // asserted where it happens rather than after another nine metres of walking.
    ch.tick_n(idle(), 120);
    CHECK(ch.state.grounded);
    CHECK(ch.state.position.y == doctest::Approx(rest_y(config(), -3.0f)).epsilon(0.05));
    CHECK_FALSE(ch.overlapping());
}

TEST_CASE("m12.2 step: step_height is read from the config, not baked in") {
    // The same 0.35 m riser, climbed under a 0.5 m budget and refused under a 0.3 m one.
    for (const bool tall_budget : {true, false}) {
        gameplay::CharacterConfig c = config();
        c.step_height = tall_budget ? 0.5f : 0.3f;
        c.snap_distance = c.step_height;

        physics::PhysicsWorld w;
        (void)add_ground(w);
        add_step(w, 0.35f);

        Character ch;
        ch.spawn(w, c, {0.0f, rest_y(c), 0.0f}, /*grounded=*/true);
        ch.tick_n(walk(0.0f, 1.0f), 120);

        if (tall_budget) {
            CHECK(ch.stats.steps_climbed > 0);
            CHECK(ch.state.position.y > rest_y(c) + 0.3f);
        } else {
            CHECK(ch.stats.steps_climbed == 0);
            CHECK(ch.state.position.y < rest_y(c) + 0.05f);
        }
    }
}
