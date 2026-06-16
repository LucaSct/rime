// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

#include "rime/core/memory/allocator.hpp"

namespace rime::core {

// A snapshot of an allocator's activity.
struct MemoryStats {
    std::size_t live_allocations = 0;  // allocate() calls not yet matched by deallocate()
    std::size_t live_bytes = 0;        // bytes currently outstanding
    std::size_t peak_bytes = 0;        // high-water mark of live_bytes
    std::size_t total_allocations = 0; // cumulative allocate() calls
};

// A decorator that wraps another Allocator and counts what flows through it: live/peak bytes,
// allocation counts, and leaks (allocations never freed). Tracking by composition - rather
// than baking counters into every allocator - keeps the allocators lean and lets tracking be
// added or removed by wrapping. Every call forwards to the inner allocator.
class TrackingAllocator final : public Allocator {
public:
    explicit TrackingAllocator(Allocator& inner) noexcept : inner_(inner) {}

    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) override;
    void deallocate(void* p, std::size_t bytes) noexcept override;

    [[nodiscard]] const MemoryStats& stats() const noexcept { return stats_; }

    // True if anything allocated through this tracker was never returned. When `log` is set
    // (the default) it logs the leak at Warn, so leaks surface in test and tool output.
    [[nodiscard]] bool report_leaks(bool log = true) const;

private:
    Allocator& inner_;
    MemoryStats stats_;
};

} // namespace rime::core
