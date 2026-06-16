// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.1 timing: Stopwatch elapsed time is non-negative and monotonic (steady_clock
// never goes backward), and a RIME_PROFILE_ZONE reports its name and a duration to the active
// zone sink when the scope closes. We avoid sleeps (slow, flaky) by doing a little
// optimizer-resistant work.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "rime/core/diagnostics/profile.hpp"

namespace {
// A loop the optimizer cannot erase (volatile accumulator), so a tiny but non-zero amount of
// time passes without us sleeping.
std::uint64_t busy_work(int iterations) {
    volatile std::uint64_t acc = 0;
    for (int i = 0; i < iterations; ++i) {
        // Simple assignment, not `+=`: C++20 deprecates compound assignment to a volatile.
        acc = acc + static_cast<std::uint64_t>(i);
    }
    return acc;
}
} // namespace

TEST_CASE("Stopwatch elapsed time is non-negative and monotonic") {
    rime::core::Stopwatch sw;
    const double t1 = sw.elapsed_us();
    busy_work(200000);
    const double t2 = sw.elapsed_us();
    CHECK(t1 >= 0.0);
    CHECK(t2 >= t1); // steady_clock never runs backward
}

TEST_CASE("a profiling zone reports its name to the active sink") {
    std::string got_name;
    double got_ms = -1.0;
    rime::core::set_zone_sink([&](std::string_view name, double ms) {
        got_name = std::string(name);
        got_ms = ms;
    });

    {
        RIME_PROFILE_ZONE("test_zone");
        busy_work(100000);
    } // zone closes here, sink fires

    CHECK(got_name == "test_zone");
    CHECK(got_ms >= 0.0);

    rime::core::set_zone_sink({});
}
