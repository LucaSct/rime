// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the turntable export's novel logic (E4): the per-frame camera yaw sequence and the numbered
// frame file naming. These are the only new pieces — each frame's actual render reuses the proven
// off-screen path (covered by mesh_offscreen_test / cap_offscreen_test), so this stays CPU-only and
// always runs in CI. See turntable.hpp and docs/math/orbit-camera.md.

#include <doctest/doctest.h>

#include <cmath>

#include "turntable.hpp"

TEST_CASE("turntable: the yaw sweep is a full, evenly-spaced 360° from the start pose (E4)") {
    using rime::viewer::turntable_yaw;
    constexpr float kTwoPi = 6.283185307179586f;
    const float yaw0 = 0.6f; // an arbitrary start azimuth
    const int n = 72;

    // Frame 0 is exactly the start pose; frame n closes the loop back onto it (a full revolution).
    CHECK(turntable_yaw(0, n, yaw0) == doctest::Approx(yaw0));
    CHECK(turntable_yaw(n, n, yaw0) == doctest::Approx(yaw0 + kTwoPi));

    // The step between consecutive frames is constant = 2π/n (evenly spaced views).
    const float step = turntable_yaw(1, n, yaw0) - turntable_yaw(0, n, yaw0);
    CHECK(step == doctest::Approx(kTwoPi / static_cast<float>(n)));
    for (int i = 1; i < n; ++i) {
        const float d = turntable_yaw(i + 1, n, yaw0) - turntable_yaw(i, n, yaw0);
        CHECK(d == doctest::Approx(step));
    }

    // Halfway through the sweep faces the opposite way (yaw0 + π).
    CHECK(turntable_yaw(n / 2, n, yaw0) == doctest::Approx(yaw0 + kTwoPi * 0.5f));

    // n is clamped to ≥ 1, so a degenerate count can't divide by zero (frame 1 lands a full turn on).
    CHECK(turntable_yaw(1, 0, yaw0) == doctest::Approx(yaw0 + kTwoPi));
}

TEST_CASE("turntable: the frame path inserts a zero-padded index on the template's stem (E4)") {
    using rime::viewer::turntable_frame_path;

    // A .ppm template: the index lands on the stem, the extension is re-appended.
    CHECK(turntable_frame_path("icem_view.ppm", 0) == "icem_view_000.ppm");
    CHECK(turntable_frame_path("icem_view.ppm", 3) == "icem_view_003.ppm");
    CHECK(turntable_frame_path("icem_view.ppm", 42) == "icem_view_042.ppm");
    CHECK(turntable_frame_path("icem_view.ppm", 123) == "icem_view_123.ppm"); // padding doesn't clip

    // No extension on the filename → the index is simply appended (still a .ppm).
    CHECK(turntable_frame_path("out/spin", 12) == "out/spin_012.ppm");

    // A dot that belongs to a directory, not the filename, is left intact.
    CHECK(turntable_frame_path("a/b.c/frame.ppm", 5) == "a/b.c/frame_005.ppm");
    CHECK(turntable_frame_path("a.b/spin", 7) == "a.b/spin_007.ppm");
}
