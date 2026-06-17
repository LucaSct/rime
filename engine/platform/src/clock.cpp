// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/platform/clock.hpp"

#include <chrono>

namespace rime::platform {

std::uint64_t Clock::now_ns() noexcept {
    // steady_clock is the standard library's monotonic clock; on every platform we target it is
    // implemented on top of the OS's monotonic source. We normalize to nanoseconds so callers
    // have a single integer unit regardless of the clock's native tick period.
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

void FrameTimer::tick() noexcept {
    const std::uint64_t now = Clock::now_ns();
    if (frames_ == 0) {
        start_ns_ = now;
        last_ns_ = now;
    }
    const std::uint64_t dt_ns = now - last_ns_;
    last_ns_ = now;
    delta_s_ = static_cast<double>(dt_ns) * 1e-9;
    ++frames_;
    if (delta_s_ > 0.0) {
        const double instantaneous = 1.0 / delta_s_;
        // Exponential moving average: a steady readout instead of a number that jitters each frame.
        smoothed_fps_ =
            smoothed_fps_ > 0.0 ? smoothed_fps_ * 0.9 + instantaneous * 0.1 : instantaneous;
    }
}

double FrameTimer::elapsed_seconds() const noexcept {
    if (frames_ == 0) {
        return 0.0;
    }
    return static_cast<double>(Clock::now_ns() - start_ns_) * 1e-9;
}

} // namespace rime::platform
