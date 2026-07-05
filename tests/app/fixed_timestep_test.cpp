// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the fixed-timestep accumulator (M5.7). Pure and clock-free — this is the intellectual
// core of the frame loop (ADR-0023 §1–2), so it is pinned in isolation before the Application wires
// it to a World: exact tick counts for clean inputs, sub-tick accumulation, the interpolation
// alpha, the spiral-of-death clamp, and the degenerate-input guards.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN // the one TU of this exe supplies doctest's main()
#include <doctest/doctest.h>

#include "rime/app/fixed_timestep.hpp"

namespace {

using rime::app::FixedTimestep;

TEST_CASE("fixed timestep: whole frames tick once, multiples tick many (M5.7)") {
    FixedTimestep ts = FixedTimestep::from_hz(60.0);
    const double fd = ts.fixed_dt;

    // Exactly one tick's worth of real time → exactly one tick, nothing left over.
    FixedTimestep::Step s = ts.advance(fd);
    CHECK(s.ticks == 1);
    CHECK(s.clamped == false);
    CHECK(s.alpha == doctest::Approx(0.0));

    // Two ticks' worth → two ticks (still under the default clamp of 8).
    s = ts.advance(2.0 * fd);
    CHECK(s.ticks == 2);
    CHECK(s.alpha == doctest::Approx(0.0));
}

TEST_CASE("fixed timestep: sub-tick frames accumulate, then fire (M5.7)") {
    FixedTimestep ts = FixedTimestep::from_hz(60.0);
    const double fd = ts.fixed_dt;

    // 0.7 of a tick: not yet — the time is banked.
    FixedTimestep::Step s = ts.advance(0.7 * fd);
    CHECK(s.ticks == 0);
    CHECK(s.alpha == doctest::Approx(0.7)); // 0.7 of the way to the next tick

    // Another 0.7 → 1.4 banked → one tick fires, 0.4 remains.
    s = ts.advance(0.7 * fd);
    CHECK(s.ticks == 1);
    CHECK(s.alpha == doctest::Approx(0.4));
}

TEST_CASE("fixed timestep: the spiral-of-death clamp bounds ticks and drops backlog (M5.7)") {
    FixedTimestep ts = FixedTimestep::from_hz(60.0);
    ts.max_ticks_per_frame = 4;
    const double fd = ts.fixed_dt;

    // A giant frame (a hitch): a hundred ticks are owed, but only max_ticks_per_frame run and the
    // rest is discarded — otherwise the debt compounds and the sim never catches up.
    const FixedTimestep::Step s = ts.advance(100.0 * fd);
    CHECK(s.ticks == 4);
    CHECK(s.clamped == true);
    CHECK(ts.accumulator < fd); // backlog dropped, not carried
    CHECK(s.alpha >= 0.0);
    CHECK(s.alpha < 1.0);

    // And the next normal frame is back to one tick — the hitch did not leave a lasting debt.
    const FixedTimestep::Step s2 = ts.advance(fd);
    CHECK(s2.ticks <= 2); // ~1, plus whatever sub-tick remainder the fmod left; never a pile-up
    CHECK(s2.clamped == false);
}

TEST_CASE("fixed timestep: degenerate inputs are inert, not explosive (M5.7)") {
    FixedTimestep ts = FixedTimestep::from_hz(120.0);
    const double fd = ts.fixed_dt;

    // Negative dt (a clock that appeared to run backwards) adds no time and runs nothing.
    ts.accumulator = 0.0;
    FixedTimestep::Step s = ts.advance(-1.0);
    CHECK(s.ticks == 0);
    CHECK(ts.accumulator == doctest::Approx(0.0));

    // A zero fixed_dt is a misconfiguration; advance must not divide by it or loop forever.
    FixedTimestep bad;
    bad.fixed_dt = 0.0;
    s = bad.advance(1.0);
    CHECK(s.ticks == 0);
    CHECK(s.alpha == doctest::Approx(0.0));

    // from_hz(0) refuses the divide and falls back to 60 Hz rather than producing an infinite step.
    const FixedTimestep safe = FixedTimestep::from_hz(0.0);
    CHECK(safe.fixed_dt == doctest::Approx(1.0 / 60.0));
    (void)fd;
}

} // namespace
