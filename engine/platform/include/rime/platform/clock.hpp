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

} // namespace rime::platform
