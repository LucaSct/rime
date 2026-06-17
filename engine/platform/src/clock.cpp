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

} // namespace rime::platform
