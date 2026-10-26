// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "rime/replication/interaction.hpp"

using namespace rime::replication;
using rime::core::Quat;
using rime::core::Vec3;

namespace {

constexpr double kDt = 1.0 / 60.0;
constexpr PredictedEntityId kCrate{5, 2};

InteractionParams hello() {
    InteractionParams p;
    p.contact_distance = 0.65f;
    p.player_max_speed = 4.0f;
    p.replay_ticks = 64;
    p.fixed_dt = kDt;
    return p;
}

InteractionSample far_rest(float d) {
    return {d, 0.0f, false};
}

// Drives a policy to member state at distance 1 m. The join tick is a MOVING one, so the rest
// counter starts at zero and "N ticks of rest" below is counted from the first far tick.
void make_member(InteractionMembershipPolicy& m) {
    REQUIRE(m.update({1.0f, 1.0f, false}) == MembershipDecision::Join);
}

} // namespace

TEST_CASE("interaction: radii equal the formula") {
    const InteractionParams p = hello();
    // 4 m/s * 64 ticks / 60 Hz = 4.2667 m of reach, plus the contact distance.
    CHECK(p.join_radius() == doctest::Approx(0.65 + 4.0 * 64.0 / 60.0).epsilon(1e-6));
    CHECK(p.leave_radius() == doctest::Approx(p.join_radius() + 1.0f));
}

TEST_CASE("interaction: oscillating +-0.5 m across the join radius does not flap") {
    InteractionMembershipPolicy m(hello());
    const float r = m.params().join_radius();
    for (int i = 0; i < 200; ++i)
        m.update(far_rest(i % 2 == 0 ? r - 0.5f : r + 0.5f));
    CHECK(m.counters().joins == 1);
    CHECK(m.counters().leaves == 0);
    CHECK(m.member());
}

TEST_CASE("interaction: leave waits exactly rest_ticks of rest") {
    InteractionMembershipPolicy m(hello());
    make_member(m);
    const float far = m.params().leave_radius() + 0.1f;
    // Ticks 1..29 of rest stay; tick 30 leaves.
    for (int i = 1; i < 30; ++i)
        CHECK(m.update(far_rest(far)) == MembershipDecision::Stay);
    CHECK(m.update(far_rest(far)) == MembershipDecision::Leave);
    CHECK_FALSE(m.member());
    CHECK(m.counters().leaves == 1);
}

TEST_CASE("interaction: a touch or motion at tick 29 resets the rest run") {
    for (const bool touch : {true, false}) {
        InteractionMembershipPolicy m(hello());
        make_member(m);
        const float far = m.params().leave_radius() + 0.1f;
        for (int i = 1; i < 29; ++i)
            CHECK(m.update(far_rest(far)) == MembershipDecision::Stay);
        CHECK(m.update({far, touch ? 0.0f : 1.0f, touch}) == MembershipDecision::Stay);
        const auto deferred =
            touch ? m.counters().leave_deferred_touching : m.counters().leave_deferred_moving;
        CHECK(deferred == 1);
        for (int i = 1; i < 30; ++i)
            CHECK(m.update(far_rest(far)) == MembershipDecision::Stay);
        CHECK(m.update(far_rest(far)) == MembershipDecision::Leave);
    }
}

TEST_CASE("interaction: a leave delayed only by the quiet window is counted as quiet") {
    InteractionMembershipPolicy m(hello());
    make_member(m);
    const float far = m.params().leave_radius() + 0.1f;
    // Ticks 1..29 of rest beyond the radius: each is a deferral for the quiet window alone.
    for (int i = 1; i < 30; ++i)
        CHECK(m.update(far_rest(far)) == MembershipDecision::Stay);
    CHECK(m.counters().leave_deferred_quiet == 29);
    CHECK(m.counters().leave_deferred_moving == 0);
    CHECK(m.counters().leave_deferred_touching == 0);
    CHECK(m.update(far_rest(far)) == MembershipDecision::Leave);
    CHECK(m.counters().leave_deferred_quiet == 29); // the leaving tick is not a deferral
    // A tick of motion beyond the radius is "moving", never "quiet"; inside the radius, nothing.
    InteractionMembershipPolicy n(hello());
    make_member(n);
    n.update({far, 1.0f, false});
    n.update(far_rest(n.params().leave_radius() - 0.01f));
    CHECK(n.counters().leave_deferred_quiet == 0);
    CHECK(n.counters().leave_deferred_moving == 1);
}

TEST_CASE("interaction: it never leaves inside the leave radius, however still") {
    InteractionMembershipPolicy m(hello());
    make_member(m);
    for (int i = 0; i < 500; ++i)
        m.update(far_rest(m.params().leave_radius() - 0.01f));
    CHECK(m.member());
}

TEST_CASE("interaction: joined_in_contact counts a join while touching") {
    InteractionMembershipPolicy m(hello());
    CHECK(m.update({0.5f, 0.0f, true}) == MembershipDecision::Join);
    CHECK(m.counters().joined_in_contact == 1);
    InteractionMembershipPolicy n(hello());
    n.update(far_rest(3.0f));
    CHECK(n.counters().joined_in_contact == 0);
}

TEST_CASE("interaction: raw bound for hello-game is about 6.4 m") {
    CHECK(interaction_raw_bound(4.0f, 12.0f, 6.0f, 64, kDt) ==
          doctest::Approx(6.0 * 64.0 / 60.0).epsilon(1e-5));
}

TEST_CASE("smoother: an offset decays to e^-1 after tau") {
    CorrectionSmoother s; // tau = 0.1 s
    s.on_correction(kCrate, Vec3{0.5f, 0, 0}, Vec3{});
    // 6 steps of 1/60 s = exactly 0.1 s; tolerance 1e-4 m on a 0.5 m offset (float exp chain).
    for (int i = 0; i < 6; ++i)
        s.advance(kDt);
    CHECK(s.offset_length(kCrate) == doctest::Approx(0.5 * std::exp(-1.0)).epsilon(1e-4));
}

TEST_CASE("smoother: snaps above the threshold, keeps the offset below it") {
    CorrectionSmoother s;
    s.on_correction(kCrate, Vec3{1.5f, 0, 0}, Vec3{});
    CHECK(s.counters().snaps == 1);
    CHECK(s.offset_length(kCrate) == 0.0f);
    const Vec3 sim{3, 0, 0};
    CHECK(s.presented(kCrate, sim).x == sim.x);
    s.on_correction(kCrate, Vec3{0.9f, 0, 0}, Vec3{});
    CHECK(s.counters().snaps == 1);
    CHECK(s.offset_length(kCrate) == doctest::Approx(0.9f));
    CHECK(s.presented(kCrate, Vec3{}).x == doctest::Approx(0.9f));
    s.forget(kCrate);
    CHECK(s.offset_length(kCrate) == 0.0f);
}

TEST_CASE("smoother: presented jump stays under the raw jump; tau -> 0 control sees it") {
    // 0.5 m correction, then 6 ticks at 60 Hz.
    CorrectionSmoother smooth;
    smooth.on_correction(kCrate, Vec3{0.5f, 0, 0}, Vec3{});
    for (int i = 0; i < 6; ++i)
        smooth.advance(kDt);
    CHECK(smooth.counters().max_raw_jump == doctest::Approx(0.5f));
    CHECK(smooth.counters().max_presented_jump < smooth.counters().max_raw_jump);
    // First step is the biggest: 0.5 * (1 - e^(-1/6)) = 0.077 m.
    CHECK(smooth.counters().max_presented_jump == doctest::Approx(0.077).epsilon(0.01));

    // Sensitivity control: with tau ~ 0 the same correction is applied at once, so the
    // presented-jump gauge must read the full raw jump -- proving it can see an unsmoothed pop.
    SmootherParams p;
    p.tau_seconds = 1e-9;
    CorrectionSmoother raw(p);
    raw.on_correction(kCrate, Vec3{0.5f, 0, 0}, Vec3{});
    for (int i = 0; i < 6; ++i)
        raw.advance(kDt);
    CHECK(raw.counters().max_presented_jump == doctest::Approx(raw.counters().max_raw_jump));
}

TEST_CASE("smoother: orientation offsets reproduce the shown pose, decay, and snap") {
    CorrectionSmoother s;
    const Quat before = rime::core::quat_from_axis_angle(Vec3{0, 1, 0}, 0.4f);
    const Quat after = rime::core::quat_identity();
    s.on_correction(kCrate, before, after);
    CHECK(rime::core::approx_eq(s.presented(kCrate, after), before, 1e-5f));
    for (int i = 0; i < 6; ++i)
        s.advance(kDt);
    const Quat later = s.presented(kCrate, after);
    // Angle should have decayed to 0.4 / e.
    CHECK(2.0 * std::acos(std::abs(later.w)) == doctest::Approx(0.4 / std::exp(1.0)).epsilon(1e-3));
    s.on_correction(kCrate, rime::core::quat_from_axis_angle(Vec3{0, 1, 0}, 2.0f), after);
    CHECK(s.counters().rotation_snaps == 1);
    CHECK(rime::core::approx_eq(s.presented(kCrate, after), after));
}
