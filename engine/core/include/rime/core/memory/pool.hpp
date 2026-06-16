// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

#include "rime/core/memory/allocator.hpp"

namespace rime::core {

// A fixed-size block pool. It carves a buffer into equal-sized blocks and hands them out O(1)
// from a free-list that is threaded *through the free blocks themselves* (each free block
// stores a pointer to the next), so the pool needs no side table. allocate() returns one
// block; deallocate() pushes it back. This is the allocator of choice for many same-sized
// objects (particles, components, slot-map nodes).
class PoolAllocator final : public Allocator {
public:
    // Carve [buffer, buffer + capacity) into blocks of (at least) `block_size` bytes, each
    // aligned to `block_align` (a power of two). The effective block size is rounded up so
    // every block stays aligned and is large enough to hold the free-list link.
    PoolAllocator(void* buffer,
                  std::size_t capacity,
                  std::size_t block_size,
                  std::size_t block_align = alignof(std::max_align_t)) noexcept;

    // Serve one block. `bytes`/`alignment` are only checked to fit a block (every block is
    // identical); returns nullptr when the pool is exhausted or the request can't fit.
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) override;

    void deallocate(void* p, std::size_t bytes) noexcept override;

    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }

    [[nodiscard]] std::size_t free_blocks() const noexcept { return free_count_; }

    [[nodiscard]] std::size_t capacity_blocks() const noexcept { return block_count_; }

private:
    // A free block, reinterpreted as a node in an intrusive singly-linked free-list. The
    // block's own storage holds this link while it is free; user data overwrites it while
    // allocated. The two never overlap in time, so the aliasing is safe.
    struct FreeNode {
        FreeNode* next;
    };

    std::byte* base_;
    std::size_t block_size_;
    std::size_t block_align_;
    std::size_t block_count_;
    std::size_t free_count_;
    FreeNode* free_head_;
};

} // namespace rime::core
