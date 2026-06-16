// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

#include "rime/core/memory/allocator.hpp"

namespace rime::core {

// A linear ("bump pointer") allocator. It owns a fixed buffer and serves memory by advancing
// an offset, with no per-allocation bookkeeping, so an allocation is a few instructions. The
// trade-off: an individual deallocate() is a no-op; you reclaim everything at once with
// reset(). Ideal for per-frame or per-pass scratch memory where many short-lived allocations
// share one lifetime.
class ArenaAllocator final : public Allocator {
public:
    // Adopt an externally-owned buffer [buffer, buffer + capacity). The arena neither owns nor
    // frees the buffer; the caller controls its lifetime.
    ArenaAllocator(void* buffer, std::size_t capacity) noexcept;

    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) override;

    // No-op: an arena frees in bulk via reset(). Present only to satisfy the interface.
    void deallocate(void* p, std::size_t bytes) noexcept override;

    // Forget all allocations, making the whole buffer available again. O(1); does not touch
    // the bytes, so callers must have destroyed any objects living in them first.
    void reset() noexcept;

    [[nodiscard]] std::size_t used() const noexcept { return offset_; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::byte* base_;
    std::size_t capacity_;
    std::size_t offset_;
};

} // namespace rime::core
