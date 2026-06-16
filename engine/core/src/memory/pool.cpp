// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/memory/pool.hpp"

namespace rime::core {

PoolAllocator::PoolAllocator(void* buffer,
                             std::size_t capacity,
                             std::size_t block_size,
                             std::size_t block_align) noexcept
    : base_(static_cast<std::byte*>(buffer)), block_size_(0), block_align_(block_align),
      block_count_(0), free_count_(0), free_head_(nullptr) {
    // Each block must hold the free-list link and stay aligned, so the effective block size is
    // at least sizeof(FreeNode), rounded up to a multiple of the alignment.
    std::size_t bs = block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size;
    block_size_ = align_up(bs, block_align);

    // Align the first block within the buffer, then count how many whole blocks fit in what
    // remains after that head padding.
    auto* start = static_cast<std::byte*>(align_ptr(base_, block_align));
    const std::size_t head_pad = static_cast<std::size_t>(start - base_);
    const std::size_t usable = capacity > head_pad ? capacity - head_pad : 0;
    block_count_ = usable / block_size_;
    free_count_ = block_count_;

    // Thread the free-list through the blocks. Build it back-to-front so the lowest-address
    // block ends up at the head (the first one handed out) - tidy and cache-friendly.
    for (std::size_t i = block_count_; i-- > 0;) {
        auto* node = reinterpret_cast<FreeNode*>(start + i * block_size_);
        node->next = free_head_;
        free_head_ = node;
    }
}

void* PoolAllocator::allocate(std::size_t bytes, std::size_t alignment) {
    // Every block is identical, so we only check the request fits one. Alignment is satisfied
    // up to block_align_ (the boundary every block was carved on).
    if (bytes > block_size_ || alignment > block_align_ || free_head_ == nullptr) {
        return nullptr;
    }
    FreeNode* node = free_head_;
    free_head_ = node->next;
    --free_count_;
    return node;
}

void PoolAllocator::deallocate(void* p, std::size_t /*bytes*/) noexcept {
    if (p == nullptr) {
        return;
    }
    // Push the block back onto the free-list. We trust p came from this pool; a pool has no
    // cheap way to validate ownership, so passing a foreign pointer is a caller error.
    auto* node = static_cast<FreeNode*>(p);
    node->next = free_head_;
    free_head_ = node;
    ++free_count_;
}

} // namespace rime::core
