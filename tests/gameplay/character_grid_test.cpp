// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

#include "character_fixture.hpp"

// m12.2 proofs: THE PROBE GRID — a thousand-odd configurations, no expected trajectories.
//
// The cases in the other files assert what the controller SHOULD do in situations chosen to have
// closed-form answers. This one asserts only what must be true in EVERY situation, across a sweep
// of slope angles, input headings and step heights that nobody chose one at a time. It is the net
// under the specific proofs: a controller can pass every hand-written case and still explode in
// the configuration nobody thought to write down, and the m12.1 history says the explosions come
// from geometry that is merely awkward rather than exotic.
//
// The invariants are STRUCTURAL, and each is a property the design promises rather than a number
// that happened to come out:
//
//   * nothing is ever NaN;
//   * the character never ends a tick overlapping the world (its own recovery pass is the only
//     mechanism allowed to be behind that, and it must actually work);
//   * the character never leaves the world downward — a fall through the floor is the failure this
//     whole brick exists to prevent;
//   * `grounded` is never asserted without a walkable surface actually being under the capsule;
//   * speed stays inside what gravity and max_speed can account for;
//   * every give-up counter stays inside a per-tick bound, so a controller that survives by
//     silently skipping its work fails here rather than passing quietly (guardrail 5's rule, and
//     the reason StepStats exists at all).
//
// A failing configuration PRINTS ITSELF (the probe-before-theorising workflow): the point of a
// grid is to hand back a reproduction, not a boolean.
using namespace rime_test;

namespace {

constexpr float kPi = 3.14159265358979f;

struct GridResult {
    bool nan_seen = false;
    bool overlapped = false;
    bool fell_through = false;
    bool false_ground = false;
    bool too_fast = false;
    bool counter_runaway = false;
    float lowest_feet = 0.0f;
    float fastest = 0.0f;
};

// One configuration: a floor, a ramp of `slope` rising toward +X, and a step of `step_height`
// across the -Z side. The character starts on the flat and walks along `heading` for `kTicks`.
// Whether it meets the ramp, the step, both or neither is what the heading axis sweeps.
// 70 ticks is ~7 m of travel. The floor is 20 m across and the character starts 8 m from its
// nearest edge, so the sweep can never walk OFF the world — which would be a legitimate fall and
// would make the "never leaves the world downward" invariant untestable.
constexpr int kTicks = 70;

[[nodiscard]] GridResult run_case(float slope, float heading, float step_height) {
    physics::PhysicsWorld w;
    (void)add_ground(w);
    if (slope > 0.01f) {
        (void)add_ramp(w, slope, {4.0f, 0.4f, 4.0f});
    }
    if (step_height > 0.01f) {
        (void)add_static_box(
            w, {8.0f, step_height * 0.5f, 3.0f}, {0.0f, step_height * 0.5f, -6.0f});
    }

    gameplay::CharacterConfig cfg;
    cfg.step_height = 0.3f;
    cfg.snap_distance = 0.3f;

    Character ch;
    ch.spawn(w, cfg, {-2.0f, rest_y(cfg), 2.0f}, /*grounded=*/true);

    const gameplay::CharacterInput in = walk(std::sin(heading), std::cos(heading));

    GridResult r;
    r.lowest_feet = ch.feet_y();
    int airborne_ticks = 0;

    for (int t = 0; t < kTicks; ++t) {
        ch.tick(in);
        if (!ch.state.grounded) {
            ++airborne_ticks;
        }

        if (!finite(ch.state.position) || !finite(ch.state.velocity) ||
            !finite(ch.state.ground_normal)) {
            r.nan_seen = true;
            break; // everything downstream of a NaN is noise
        }
        r.lowest_feet = std::min(r.lowest_feet, ch.feet_y());
        r.fastest = std::max(r.fastest, core::length(ch.state.velocity));

        if (ch.overlapping()) {
            r.overlapped = true;
        }
        // Falling out of the world. The floor's top is y = 0 and it is 20 m across, so anything
        // this far below it has gone through solid geometry rather than off an edge.
        if (ch.feet_y() < -1.0f) {
            r.fell_through = true;
        }
        // `grounded` is a claim about the world, so check it against the world: a walkable surface
        // must actually be under the capsule, within the reach the snap is allowed to use.
        if (ch.state.grounded) {
            physics::Ray down;
            down.origin = ch.state.position;
            down.direction = {0.0f, -1.0f, 0.0f};
            down.max_distance = cfg.half_height + cfg.radius + cfg.snap_distance + 4.0f * cfg.skin;
            physics::RayHit hit;
            physics::QueryFilter f;
            f.exclude = ch.body;
            if (!w.raycast(down, hit, f) ||
                core::dot(hit.normal, kUp) < cfg.max_slope_cos - 1e-3f) {
                r.false_ground = true;
            }
        }
    }

    // Speed accounting: horizontal is capped by max_speed, vertical by however long gravity had.
    const float ceiling =
        cfg.max_speed + cfg.gravity * kDt * static_cast<float>(airborne_ticks + 1) + 1e-2f;
    if (r.fastest > ceiling) {
        r.too_fast = true;
    }

    // Counter bounds. Every one of these is per-tick work, so a total beyond ticks * per-tick
    // budget means a loop ran away rather than a path being taken more often than expected.
    const auto& s = ch.stats;
    const std::uint32_t ticks = static_cast<std::uint32_t>(kTicks);
    if (s.slide_iterations > ticks * static_cast<std::uint32_t>(cfg.max_slide_iterations) ||
        s.slide_exhausted > ticks || s.snaps > ticks || s.stuck > 4u * ticks ||
        s.corner_locked > ticks || s.steps_climbed > ticks || s.depenetrations > 2u * ticks) {
        r.counter_runaway = true;
    }
    return r;
}

} // namespace

TEST_CASE("m12.2 grid: structural invariants hold across slope x heading x step height") {
    int configs = 0;
    int failures = 0;

    for (int slope_deg = 0; slope_deg <= 85; slope_deg += 5) {
        for (int h = 0; h < 12; ++h) {
            for (const float step_height : {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.6f}) {
                const float slope = static_cast<float>(slope_deg) * kPi / 180.0f;
                const float heading = static_cast<float>(h) * (2.0f * kPi / 12.0f);
                const GridResult r = run_case(slope, heading, step_height);
                ++configs;

                const bool ok = !r.nan_seen && !r.overlapped && !r.fell_through &&
                                !r.false_ground && !r.too_fast && !r.counter_runaway;
                if (!ok) {
                    ++failures;
                    // Hand back a reproduction, not a boolean.
                    std::printf("GRID FAIL slope=%d deg heading=%d/12 step=%.2f | nan=%d "
                                "overlap=%d fell=%d false_ground=%d fast=%d counters=%d "
                                "(lowest_feet=%.4f fastest=%.4f)\n",
                                slope_deg,
                                h,
                                static_cast<double>(step_height),
                                r.nan_seen ? 1 : 0,
                                r.overlapped ? 1 : 0,
                                r.fell_through ? 1 : 0,
                                r.false_ground ? 1 : 0,
                                r.too_fast ? 1 : 0,
                                r.counter_runaway ? 1 : 0,
                                static_cast<double>(r.lowest_feet),
                                static_cast<double>(r.fastest));
                }
            }
        }
    }

    CHECK(configs == 18 * 12 * 6); // vacuity: the sweep really ran
    CHECK(failures == 0);
}
