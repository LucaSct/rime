// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>

// The fixed-timestep accumulator (M5.7, ADR-0023 §1–2): turn a variable real-frame `dt` into a
// whole number of equal-sized simulation ticks plus a [0, 1) interpolation alpha, clamped against
// the spiral of death. This is the standard "Fix Your Timestep" accumulator (Glenn Fiedler), and
// it is the seam multiplayer (M11) is built on: because a tick always advances the world by the
// SAME `fixed_dt`, the world's state after k ticks is a pure function of k — independent of how
// wall-clock time was chopped into frames. That determinism is what lockstep/rollback netcode
// needs, and un-baking a variable `dt` out of gameplay systems later would be a rewrite.
//
// Deliberately pure and clock-free: the loop feeds it real `dt`; a test feeds it whatever pattern
// it likes. No World, no device, no platform clock — so the accumulator logic is verifiable in
// isolation (tests/app/fixed_timestep_test.cpp). Derivation and the determinism argument:
// ADR-0023; the loop that drives it is Application (application.hpp).
namespace rime::app {

struct FixedTimestep {
    double fixed_dt = 1.0 / 60.0; // seconds the world advances per simulation tick (60 Hz default)
    int max_ticks_per_frame = 8;  // spiral-of-death clamp: never run more than this many per frame
    double accumulator = 0.0;     // unspent real time carried between frames

    // Build from a tick rate in Hz (the ergonomic front door; the loop stores dt, tests read Hz).
    [[nodiscard]] static FixedTimestep from_hz(double hz, int max_ticks = 8) noexcept {
        FixedTimestep ts;
        ts.fixed_dt = hz > 0.0 ? 1.0 / hz : 1.0 / 60.0;
        ts.max_ticks_per_frame = max_ticks;
        return ts;
    }

    // What one frame's worth of elapsed real time turns into.
    struct Step {
        int ticks = 0;        // fixed ticks to run this frame (0 .. max_ticks_per_frame)
        double alpha = 0.0;   // [0, 1): fraction of a tick left in the accumulator — how far
                              // between the last tick and the next the render frame sits
        bool clamped = false; // the spiral-of-death clamp fired and dropped backlog this frame
    };

    // Advance by one frame's real elapsed time and report the ticks to run + the render alpha.
    // A monotonic clock never goes backwards, but a caller bug shouldn't rewind the sim, so a
    // non-positive `frame_dt` contributes no time (it can still flush a pending tick from an
    // earlier accumulation — advance(0) is the "don't add time, just ask" query).
    [[nodiscard]] Step advance(double frame_dt) noexcept {
        // A non-positive fixed_dt is a misconfiguration; refuse to divide by it or loop forever.
        if (fixed_dt <= 0.0) {
            return {};
        }
        if (frame_dt > 0.0) {
            accumulator += frame_dt;
        }

        Step step;
        while (accumulator >= fixed_dt && step.ticks < max_ticks_per_frame) {
            accumulator -= fixed_dt;
            ++step.ticks;
        }

        // Hit the clamp with time still owed: drop the backlog rather than let it snowball into an
        // ever-growing debt (the spiral of death). Simulation time slows relative to wall-clock;
        // it never freezes. Pulling the accumulator into [0, fixed_dt) also keeps alpha meaningful.
        if (accumulator >= fixed_dt) {
            accumulator = std::fmod(accumulator, fixed_dt);
            step.clamped = true;
        }

        step.alpha = accumulator / fixed_dt;
        return step;
    }
};

} // namespace rime::app
