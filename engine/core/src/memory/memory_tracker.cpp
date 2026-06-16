// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/memory/memory_tracker.hpp"

#include "rime/core/diagnostics/log.hpp"

namespace rime::core {

void* TrackingAllocator::allocate(std::size_t bytes, std::size_t alignment) {
    void* p = inner_.allocate(bytes, alignment);
    if (p != nullptr) {
        ++stats_.live_allocations;
        ++stats_.total_allocations;
        stats_.live_bytes += bytes;
        if (stats_.live_bytes > stats_.peak_bytes) {
            stats_.peak_bytes = stats_.live_bytes;
        }
    }
    return p;
}

void TrackingAllocator::deallocate(void* p, std::size_t bytes) noexcept {
    if (p == nullptr) {
        return;
    }
    inner_.deallocate(p, bytes);
    // A correct caller passes the same size it allocated; clamp anyway so a mismatch can't
    // underflow the counters into huge values.
    if (stats_.live_allocations > 0) {
        --stats_.live_allocations;
    }
    stats_.live_bytes = bytes <= stats_.live_bytes ? stats_.live_bytes - bytes : 0;
}

bool TrackingAllocator::report_leaks(bool log) const {
    const bool leaked = stats_.live_allocations > 0;
    if (leaked && log) {
        RIME_WARN("memory leak: {} allocation(s), {} byte(s) still live",
                  stats_.live_allocations,
                  stats_.live_bytes);
    }
    return leaked;
}

} // namespace rime::core
