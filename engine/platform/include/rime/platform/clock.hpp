// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

// A monotonic, high-resolution clock for frame timing and profiling.
//
// We deliberately build on std::chrono::steady_clock rather than calling the OS timer APIs
// directly. steady_clock is *already* a thin wrapper over each platform's monotonic source
// (QueryPerformanceCounter on Windows, mach_absolute_time on macOS, clock_gettime(MONOTONIC)
// on Linux) — going native here would re-implement the standard library for no measurable
// gain. "Complete native control" is reserved for windowing/input, where the OS APIs differ in
// capability, not for a monotonic counter, where they do not. (See ADR-0006.)
namespace rime::platform {

class Clock {
public:
    // Nanoseconds since an unspecified but fixed epoch. Monotonic: never runs backwards and
    // never jumps with wall-clock / NTP adjustments, so only *differences* are meaningful.
    [[nodiscard]] static std::uint64_t now_ns() noexcept;
};

// Per-frame timing built on Clock: the delta time the simulation integrates with, plus a smoothed
// FPS readout and a frame counter. Call tick() once per frame; the first call only establishes the
// baseline (its delta is 0).
class FrameTimer {
public:
    void tick() noexcept;

    [[nodiscard]] double delta_seconds() const noexcept { return delta_s_; } // last frame's dt

    [[nodiscard]] double elapsed_seconds() const noexcept; // since the first tick

    [[nodiscard]] double smoothed_fps() const noexcept { return smoothed_fps_; }

    [[nodiscard]] std::uint64_t frame_count() const noexcept { return frames_; }

private:
    std::uint64_t start_ns_ = 0;
    std::uint64_t last_ns_ = 0;
    double delta_s_ = 0.0;
    double smoothed_fps_ = 0.0;
    std::uint64_t frames_ = 0;
};

} // namespace rime::platform
