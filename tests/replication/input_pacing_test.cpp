// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>

#include "rime/replication/input_pacing.hpp"

using namespace rime::replication;

namespace {

// Drives the pacer like the client does: one report then one tick per step, reports 1 tick apart.
struct Rig {
    InputPacer pacer;
    std::uint64_t server_tick = 0;
    std::uint32_t stalls = 0;

    double step(std::uint8_t margin) {
        pacer.on_report({++server_tick, margin, stalls});
        return pacer.tick();
    }
};

} // namespace

TEST_CASE("pacer: steady margin 3 drains two ticks of buffer, 32 dilated ticks per window") {
    Rig rig;
    std::uint64_t dilated = 0;
    // Reports 1..60 complete window 1 (the 60th report arrives before that step's tick()).
    for (int i = 0; i < 60; ++i) {
        const double scale = rig.step(3);
        if (scale != 1.0) {
            ++dilated;
            CHECK(scale == doctest::Approx(1.0 + 1.0 / 16.0));
        }
        CHECK(scale >= 1.0);
    }
    CHECK(rig.pacer.windows() == 1);
    CHECK(rig.pacer.drains() == 1);
    CHECK(rig.pacer.drained_ticks_of_buffer() == 2);
    // The tick() of the 60th step already belongs to the episode.
    for (int i = 0; i < 31; ++i) {
        CHECK(rig.step(3) == doctest::Approx(1.0 + 1.0 / 16.0));
        ++dilated;
    }
    CHECK(dilated == 32);
    CHECK(rig.pacer.dilated_ticks() == 32);
    // Reports during the episode did not accumulate a window.
    CHECK(rig.pacer.windows() == 1);
    CHECK(rig.step(3) == 1.0);
    CHECK(rig.pacer.reports() == 92);
    // A second window completes after 60 more fresh reports: exactly one more episode.
    for (int i = 0; i < 59; ++i) {
        (void)rig.step(3);
    }
    CHECK(rig.pacer.windows() == 2);
    CHECK(rig.pacer.drains() == 2);
    CHECK(rig.pacer.dilated_ticks() == 33); // the second episode's first tick ran in the loop
}

TEST_CASE("pacer: a margin at or below drain_above never drains") {
    Rig rig;
    for (int i = 0; i < 300; ++i) {
        CHECK(rig.step(i % 2 == 0 ? 1 : 0) == 1.0);
    }
    CHECK(rig.pacer.windows() == 5);
    CHECK(rig.pacer.drains() == 0);
    CHECK(rig.pacer.dilated_ticks() == 0);
}

TEST_CASE("pacer: the window minimum decides, one low sample blocks the drain") {
    Rig rig;
    for (int i = 0; i < 60; ++i) {
        (void)rig.step(i == 30 ? 1 : 4);
    }
    CHECK(rig.pacer.windows() == 1);
    CHECK(rig.pacer.drains() == 0);
}

TEST_CASE("pacer: a stall rise inside a window cancels the drain") {
    Rig rig;
    (void)rig.step(3); // the first report only seeds the stall baseline
    for (int i = 1; i < 60; ++i) {
        if (i == 20) {
            ++rig.stalls;
        }
        CHECK(rig.step(3) == 1.0);
    }
    CHECK(rig.pacer.windows() == 1);
    CHECK(rig.pacer.windows_with_stall() == 1);
    CHECK(rig.pacer.drains() == 0);
    // The next window is clean and does drain: the stall count itself is not held against it.
    for (int i = 0; i < 60; ++i) {
        (void)rig.step(3);
    }
    CHECK(rig.pacer.drains() == 1);
}

TEST_CASE("pacer: a stale or duplicate report is counted and ignored") {
    InputPacer pacer;
    pacer.on_report({10, 3, 0});
    pacer.on_report({10, 0, 5});
    pacer.on_report({9, 0, 5});
    CHECK(pacer.reports() == 1);
    CHECK(pacer.stale_reports() == 2);
    CHECK(pacer.last_margin() == 3);
    CHECK(pacer.params().window_reports == 60);
}

TEST_CASE("pacer: a high margin drains at most sixteen ticks of buffer per episode") {
    Rig rig;
    for (int i = 0; i < 60; ++i) {
        (void)rig.step(63);
    }
    CHECK(rig.pacer.drains() == 1);
    CHECK(rig.pacer.drained_ticks_of_buffer() == 16);
    for (int i = 0; i < 255; ++i) {
        CHECK(rig.step(63) == doctest::Approx(1.0 + 1.0 / 16.0));
    }
    CHECK(rig.pacer.dilated_ticks() == 256);
    CHECK(rig.step(63) == 1.0);
    for (int i = 0; i < 59; ++i) {
        (void)rig.step(63);
    }
    CHECK(rig.pacer.drains() == 2);
    CHECK(rig.pacer.drained_ticks_of_buffer() == 32);
}

TEST_CASE("pacer: a stall rise during drain cancels the remaining dilation") {
    Rig rig;
    for (int i = 0; i < 60; ++i) {
        (void)rig.step(3);
    }
    REQUIRE(rig.pacer.drains() == 1);
    REQUIRE(rig.pacer.dilated_ticks() == 1);
    ++rig.stalls;
    CHECK(rig.step(3) == 1.0);
    CHECK(rig.pacer.drains_cancelled() == 1);
    CHECK(rig.pacer.dilated_ticks() == 1);
    for (int i = 0; i < 60; ++i) {
        (void)rig.step(3);
    }
    CHECK(rig.pacer.drains() == 2);
    CHECK(rig.pacer.drains_cancelled() == 1);
}
