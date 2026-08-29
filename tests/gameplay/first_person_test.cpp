// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.3a — the first-person view. Analytic, GPU-free, no world.
//
// The case that earns this file is the LAST one: the view's forward and the mover's forward are
// two independently written trig expressions of the same angle, in two modules, and if they ever
// disagree the symptom is a character that strafes when the player pushes forward. That is a bug
// you feel immediately and cannot see in any state dump, so it is asserted directly rather than
// left to a play session.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/first_person.hpp"

using namespace rime;
using namespace rime::gameplay;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// The forward basis `step_character` builds from a yaw (character.cpp: "forward = (−sin, 0,
// −cos)"). Written out here on purpose: this test is only worth something if it re-derives the
// mover's convention independently instead of calling the mover's own helper.
[[nodiscard]] core::Vec3 mover_forward(float yaw) noexcept {
    return {-std::sin(yaw), 0.0f, -std::cos(yaw)};
}

} // namespace

TEST_CASE("m13.3a: a fresh view looks down -Z, the engine's forward") {
    const FirstPersonView v{};
    const core::Vec3 dir = look_direction(v);
    CHECK(dir.x == doctest::Approx(0.0f));
    CHECK(dir.y == doctest::Approx(0.0f));
    CHECK(dir.z == doctest::Approx(-1.0f));

    // And the transform's rotation agrees with the directly-computed direction — the two must,
    // because a weapon ray uses one and the camera uses the other.
    const core::Transform tf = eye_transform(v, {0.0f, 0.0f, 0.0f});
    const core::Vec3 rotated = core::rotate(tf.rotation, {0.0f, 0.0f, -1.0f});
    CHECK(rotated.x == doctest::Approx(dir.x));
    CHECK(rotated.y == doctest::Approx(dir.y));
    CHECK(rotated.z == doctest::Approx(dir.z));
}

TEST_CASE("m13.3a: the eye sits above the character ORIGIN, not its feet") {
    FirstPersonView v{};
    v.eye_height = 0.6f;
    const core::Vec3 pos{3.0f, 1.5f, -2.0f};
    const core::Transform tf = eye_transform(v, pos);

    // CharacterState::position is the capsule's CENTRE (its own doc comment says so), so the eye is
    // position + eye_height and nothing else. Treating it as the feet is the classic first-person
    // bug: correct standing still, and the camera sinks through the floor the moment the capsule's
    // config changes.
    CHECK(tf.translation.x == doctest::Approx(pos.x));
    CHECK(tf.translation.y == doctest::Approx(pos.y + 0.6f));
    CHECK(tf.translation.z == doctest::Approx(pos.z));
}

TEST_CASE("m13.3a: looking obeys the non-inverted convention and clamps pitch") {
    SUBCASE("right turns right, and pointer-down looks down") {
        FirstPersonView v{};
        apply_look(v, /*dx=*/100.0f, /*dy=*/0.0f);
        // Turning right means the look direction acquires +X... at yaw 0, forward is −Z and right
        // is +X (the mover's own basis), so a right turn must move forward toward +X.
        CHECK(look_direction(v).x > 0.0f);
        CHECK(v.yaw < 0.0f);

        FirstPersonView d{};
        apply_look(d, 0.0f, /*dy=*/100.0f);
        CHECK(d.pitch < 0.0f);
        CHECK(look_direction(d).y < 0.0f);
    }

    SUBCASE("pitch clamps short of the pole, in both directions") {
        FirstPersonView up{};
        for (int i = 0; i < 200; ++i) {
            apply_look(up, 0.0f, -1000.0f);
        }
        CHECK(up.pitch == doctest::Approx(kMaxPitch));
        CHECK(kMaxPitch < kPi * 0.5f); // strictly short of straight up

        FirstPersonView down{};
        for (int i = 0; i < 200; ++i) {
            apply_look(down, 0.0f, 1000.0f);
        }
        CHECK(down.pitch == doctest::Approx(-kMaxPitch));

        // The reason for the clamp: at the pole the look direction is collinear with world +Y and
        // the view basis has no defined roll. Just short of it, it is not.
        const core::Vec3 dir = look_direction(up);
        CHECK(std::fabs(dir.y) < 0.9999f);
    }

    SUBCASE("yaw wraps instead of growing without bound") {
        FirstPersonView v{};
        for (int i = 0; i < 5000; ++i) {
            apply_look(v, 100.0f, 0.0f);
        }
        CHECK(v.yaw > -kPi);
        CHECK(v.yaw <= doctest::Approx(kPi));
    }

    SUBCASE("a non-finite delta is dropped, not accumulated") {
        // One NaN from a device or a replay would otherwise poison yaw permanently — every later
        // frame adds to NaN — and the symptom is a black screen rather than a traceable number.
        FirstPersonView v{};
        apply_look(v, 0.5f, 0.25f);
        const FirstPersonView before = v;
        apply_look(v, std::numeric_limits<float>::quiet_NaN(), 0.0f);
        apply_look(v, 0.0f, std::numeric_limits<float>::infinity());
        CHECK(v.yaw == doctest::Approx(before.yaw));
        CHECK(v.pitch == doctest::Approx(before.pitch));
        CHECK(std::isfinite(v.yaw));
        CHECK(std::isfinite(v.pitch));
    }
}

TEST_CASE("m13.3a: the view's forward IS the mover's forward — the one that must not drift") {
    // Two independently written trig expressions of the same angle, in two modules. If they
    // disagree the character strafes when the player pushes forward: instantly obvious to play,
    // invisible in every state dump, and impossible to catch anywhere but here.
    for (int i = -18; i <= 18; ++i) {
        const float yaw = static_cast<float>(i) * (kPi / 18.0f);
        FirstPersonView v{};
        v.yaw = yaw;
        v.pitch = 0.0f;

        const core::Vec3 view = look_direction(v);
        const core::Vec3 mover = mover_forward(yaw);
        CHECK(view.x == doctest::Approx(mover.x).epsilon(1e-5));
        CHECK(view.y == doctest::Approx(mover.y).epsilon(1e-5));
        CHECK(view.z == doctest::Approx(mover.z).epsilon(1e-5));
    }

    SUBCASE("and pitching does not disturb the heading it walks along") {
        // The mover ignores pitch entirely (character.hpp says so). So the view's HORIZONTAL
        // heading must survive looking up and down, or aiming at the sky would veer the walk.
        FirstPersonView v{};
        v.yaw = 0.7f;
        for (const float pitch : {-1.0f, -0.3f, 0.0f, 0.4f, 1.2f}) {
            v.pitch = pitch;
            const core::Vec3 dir = look_direction(v);
            const float horizontal = std::sqrt(dir.x * dir.x + dir.z * dir.z);
            REQUIRE(horizontal > 1e-3f);
            const core::Vec3 mover = mover_forward(v.yaw);
            CHECK(dir.x / horizontal == doctest::Approx(mover.x).epsilon(1e-5));
            CHECK(dir.z / horizontal == doctest::Approx(mover.z).epsilon(1e-5));
        }
    }
}

TEST_CASE("m13.3a: a look tape replays bit-identically") {
    // The same discipline m12.2 put on the mover: same tape twice ⇒ identical trajectory. The view
    // is presentation and is not rewound by reconciliation, but it IS sampled onto the input tape
    // each tick, so a view that drifted between replays would put different angles on the wire.
    std::vector<std::pair<float, float>> tape;
    for (int i = 0; i < 400; ++i) {
        tape.emplace_back(static_cast<float>((i * 37) % 71) - 35.0f,
                          static_cast<float>((i * 13) % 29) - 14.0f);
    }

    const auto play = [&tape]() {
        FirstPersonView v{};
        for (const auto& [dx, dy] : tape) {
            apply_look(v, dx, dy);
        }
        return v;
    };

    const FirstPersonView a = play();
    const FirstPersonView b = play();
    CHECK(a.yaw == b.yaw);     // bit-identical, not approximately
    CHECK(a.pitch == b.pitch); // …
    CHECK(std::isfinite(a.yaw));
}
