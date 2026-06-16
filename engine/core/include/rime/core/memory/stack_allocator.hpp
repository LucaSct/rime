// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

#include "rime/core/memory/allocator.hpp"

namespace rime::core {

// A stack allocator: a bump allocator that also frees in LIFO order via markers. Take a
// marker (the current top), allocate, then rewind to the marker to free in one O(1) step
// everything allocated after it. This fits nested scratch scopes - a subsystem grabs a marker
// on entry and rewinds on exit - while keeping bump-allocation speed.
class StackAllocator final : public Allocator {
public:
    // An opaque position in the stack, returned by mark() and consumed by rewind().
    using Marker = std::size_t;

    StackAllocator(void* buffer, std::size_t capacity) noexcept;

    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) override;

    // No-op: free in LIFO bulk via rewind(). (Per-allocation free would need per-allocation
    // headers, which defeats the purpose.)
    void deallocate(void* p, std::size_t bytes) noexcept override;

    [[nodiscard]] Marker mark() const noexcept { return offset_; }

    // Free everything allocated since `marker` was taken. The marker must come from this
    // allocator; rewinding "forward" (to a marker beyond the current top) is ignored.
    void rewind(Marker marker) noexcept;

    void reset() noexcept;

    [[nodiscard]] std::size_t used() const noexcept { return offset_; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::byte* base_;
    std::size_t capacity_;
    std::size_t offset_;
};

} // namespace rime::core
