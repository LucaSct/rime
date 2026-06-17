// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M2.1's monotonic clock. Defining the macro below makes this translation unit also
// provide doctest's main(), so the rime_platform_tests executable is self-contained (doctest is
// header-only and ships no prebuilt main()). Exactly one file per executable defines it.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "rime/platform/clock.hpp"

TEST_CASE("Clock::now_ns is monotonic and nonzero") {
    using rime::platform::Clock;
    const std::uint64_t a = Clock::now_ns();
    const std::uint64_t b = Clock::now_ns();
    CHECK(a != 0);
    // steady_clock never runs backwards, so back-to-back reads are non-decreasing.
    CHECK(b >= a);
}
