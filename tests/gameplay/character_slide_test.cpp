// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>

#include "character_fixture.hpp"

// m12.2 proofs: SLIDING — what a wall does to a velocity, and what two or three walls do.
//
// The clip is exact arithmetic: `v -= n * dot(v, n)` removes precisely the component along the
// surface normal, so the assertions below are relative-ULP bounds rather than tolerances. That is
// only possible because the controller maintains velocity ANALYTICALLY through the same plane
// clips it applies to the displacement, never re-deriving it as (distance moved / dt) — which
// would smear the query's residual into every subsequent tick.
using namespace rime_test;

namespace {

gameplay::CharacterConfig config() {
    return gameplay::CharacterConfig{};
}

// A wall whose inward face normal is `-n`: a big thin box pushed `distance` along n from the
// origin, so a character at the origin walking along n runs into it.
void add_wall(physics::PhysicsWorld& w, core::Vec3 n, float distance) {
    const core::Vec3 half{
        std::fabs(n.x) > 0.5f ? 0.5f : 8.0f, 6.0f, std::fabs(n.z) > 0.5f ? 0.5f : 8.0f};
    (void)add_static_box(w, half, n * (distance + 0.5f));
}

} // namespace

TEST_CASE("m12.2 slide: a wall removes exactly the into-wall velocity and keeps the rest") {
    // Walk diagonally into an axis-aligned wall. After the contact the velocity must have NO
    // component along the wall normal — and the tangential part must survive, or the controller is
    // a stopper rather than a slider.
    physics::PhysicsWorld w;
    (void)add_ground(w);
    add_wall(w, {0.0f, 0.0f, -1.0f}, 3.0f); // face at z = -3, its normal toward us is +Z

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);

    // move (x = +1, y = +1) at yaw 0 is (+X, -Z): into the wall at 45°. 60 ticks is enough to
    // reach it and slide, and short enough that the slide never runs off the wall's end.
    ch.tick_n(walk(1.0f, 1.0f), 60);

    const float speed = core::length(ch.state.velocity);
    REQUIRE(speed > 1.0f); // vacuity: it is still moving, so "no normal component" means something
    const core::Vec3 wall_normal{0.0f, 0.0f, 1.0f};
    CHECK(std::fabs(core::dot(ch.state.velocity, wall_normal)) <= 1e-5f * speed);
    CHECK(ch.state.velocity.x > 1.0f);   // …and it is sliding along the wall, not stopped
    CHECK(ch.state.position.z > -3.0f);  // never got through it
    CHECK_FALSE(ch.overlapping());
}

TEST_CASE("m12.2 slide: the same holds at several wall orientations") {
    // Not axis-aligned: a rotated wall's normal has no exactly-representable components, so this
    // is where a controller that quietly assumed axis alignment falls over.
    for (const float deg : {15.0f, 35.0f, 70.0f}) {
        const float rad = deg * 3.14159265f / 180.0f;
        const core::Vec3 n{std::sin(rad), 0.0f, -std::cos(rad)}; // outward from the origin

        physics::PhysicsWorld w;
        (void)add_ground(w);
        (void)add_static_box(w,
                             {8.0f, 6.0f, 0.5f},
                             n * 3.5f,
                             core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -rad));

        Character ch;
        ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);

        // Approach at 45° to the wall, not head-on: a perpendicular walk has no tangential
        // component to preserve, so it would assert nothing about sliding.
        const core::Vec3 along = core::cross(kUp, n);
        const core::Vec3 wish = core::normalize(n + along);
        // 60 ticks is ~6 m of travel: past the wall contact, but not far enough along it to
        // reach the wall's end or walk off the floor — either would end the slide being measured.
        ch.tick_n(walk(wish.x, -wish.z), 60); // move_y = +1 is world -Z

        const float speed = core::length(ch.state.velocity);
        CHECK(speed > 0.5f); // vacuity: still sliding, so "no normal component" says something
        // The surviving velocity lies in the wall plane. The bound is relative because the clip is
        // exact arithmetic — the only error is the few ULP of the normal itself.
        CHECK(std::fabs(core::dot(ch.state.velocity, n)) <= 1e-4f * speed);
        CHECK_FALSE(ch.overlapping());
    }
}

TEST_CASE("m12.2 slide: a wedge funnels motion along the crease of its two walls") {
    // Two walls meeting at 90°. Walking into the corner diagonally, the only direction satisfying
    // both planes is their line of intersection — vertical here — so the horizontal velocity is
    // annihilated by the two clips.
    physics::PhysicsWorld w;
    (void)add_ground(w);
    add_wall(w, {0.0f, 0.0f, -1.0f}, 3.0f); // z = -3
    add_wall(w, {1.0f, 0.0f, 0.0f}, 3.0f);  // x = +3

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);

    const float speed_before = config().max_speed;
    ch.tick_n(walk(1.0f, 1.0f), 90); // into the corner

    // Both plane constraints hold, and nothing is left to move with.
    CHECK(core::length(horizontal_of(ch.state.velocity)) <= 1e-4f * speed_before);
    CHECK(ch.state.position.z > -3.0f);
    CHECK(ch.state.position.x < 3.0f);
    CHECK(ch.state.grounded);
    CHECK_FALSE(ch.overlapping());
    CHECK(finite(ch.state.velocity));
}

TEST_CASE("m12.2 slide: a low ceiling stops a jump without letting the head through") {
    // A ceiling is not a special case in the algorithm — it is a downward-facing plane, clipped
    // like any wall. This asserts that: the capsule's top never passes it, and the upward velocity
    // is gone after the contact tick.
    physics::PhysicsWorld w;
    (void)add_ground(w);
    const float ceiling_y = 2.2f;
    (void)add_static_box(w, {8.0f, 0.5f, 8.0f}, {0.0f, ceiling_y + 0.5f, 0.0f});

    gameplay::CharacterConfig c = config();
    c.jump_speed = 6.0f; // more than enough to reach the ceiling from a standing start

    Character ch;
    ch.spawn(w, c, {0.0f, rest_y(c), 0.0f}, /*grounded=*/true);

    float highest_top = ch.state.position.y + c.half_height + c.radius;
    ch.tick(jump());
    CHECK_FALSE(ch.state.grounded); // the jump left the ground
    for (int i = 0; i < 90; ++i) {
        ch.tick(gameplay::CharacterInput{});
        highest_top = std::max(highest_top, ch.state.position.y + c.half_height + c.radius);
        CHECK_FALSE(ch.overlapping());
    }

    // Never through the ceiling. The margin is the query residual, not a fudge: the controller
    // stops the capsule at its contact offset, so the top stays BELOW the ceiling by design and
    // this bound only has to absorb measurement error.
    CHECK(highest_top <= ceiling_y + 2.3e-4f);
    CHECK(ch.state.grounded); // …and it came back down and landed
    CHECK(ch.state.position.y == doctest::Approx(rest_y(c)).epsilon(0.02));
}

TEST_CASE("m12.2 slide: walking into a wall head-on stops without jitter") {
    // The pathology this is written against: a controller that advances to the surface, backs off,
    // re-approaches, and vibrates. Position must SETTLE.
    physics::PhysicsWorld w;
    (void)add_ground(w);
    add_wall(w, {0.0f, 0.0f, -1.0f}, 3.0f);

    Character ch;
    ch.spawn(w, config(), {0.0f, rest_y(config()), 0.0f}, /*grounded=*/true);
    ch.tick_n(walk(0.0f, 1.0f), 90); // straight in, and let it settle

    const core::Vec3 settled = ch.state.position;
    float max_wobble = 0.0f;
    for (int i = 0; i < 120; ++i) {
        ch.tick(walk(0.0f, 1.0f)); // still pushing
        max_wobble = std::max(max_wobble, core::length(ch.state.position - settled));
    }
    CHECK(max_wobble <= ch.config.skin);
    CHECK(ch.state.position.z > -3.0f);
    CHECK_FALSE(ch.overlapping());
}
