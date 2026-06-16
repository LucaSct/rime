// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/memory/stack_allocator.hpp"

#include "rime/core/diagnostics/assert.hpp"

namespace rime::core {

StackAllocator::StackAllocator(void* buffer, std::size_t capacity) noexcept
    : base_(static_cast<std::byte*>(buffer)), capacity_(capacity), offset_(0) {}

void* StackAllocator::allocate(std::size_t bytes, std::size_t alignment) {
    // Same bump logic as the arena (see arena.cpp); the difference is rewind(), below.
    const std::size_t aligned = align_up(offset_, alignment);
    if (aligned > capacity_ || bytes > capacity_ - aligned) {
        return nullptr;
    }
    offset_ = aligned + bytes;
    return base_ + aligned;
}

void StackAllocator::deallocate(void* /*p*/, std::size_t /*bytes*/) noexcept {
    // Intentionally empty: free in LIFO bulk via rewind().
}

void StackAllocator::rewind(Marker marker) noexcept {
    // Markers are taken before the allocations they free, so a valid marker is never past the
    // current top. Assert that in debug; in any build, only ever move the top backward (never
    // forward, which would hand back memory that was never reserved).
    RIME_ASSERT(marker <= offset_);
    if (marker <= offset_) {
        offset_ = marker;
    }
}

void StackAllocator::reset() noexcept {
    offset_ = 0;
}

} // namespace rime::core
