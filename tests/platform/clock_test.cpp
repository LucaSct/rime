// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M2.1's monotonic clock. Defining the macro below makes this translation unit also
// provide doctest's main(), so the rime_platform_tests executable is self-contained (doctest is
// header-only and ships no prebuilt main()). Exactly one file per executable defines it.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include "rime/platform/clock.hpp"

TEST_CASE("Clock::now_ns is monotonic and nonzero") {
    using rime::platform::Clock;
    const std::uint64_t a = Clock::now_ns();
    const std::uint64_t b = Clock::now_ns();
    CHECK(a != 0);
    // steady_clock never runs backwards, so back-to-back reads are non-decreasing.
    CHECK(b >= a);
}

TEST_CASE("FrameTimer measures positive deltas and counts frames") {
    using rime::platform::FrameTimer;
    FrameTimer timer;

    timer.tick(); // first tick only sets the baseline; its delta is 0
    CHECK(timer.frame_count() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    timer.tick();
    CHECK(timer.frame_count() == 2);
    CHECK(timer.delta_seconds() > 0.0);
    CHECK(timer.elapsed_seconds() > 0.0);
}
