// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/memory/arena.hpp"

namespace rime::core {

ArenaAllocator::ArenaAllocator(void* buffer, std::size_t capacity) noexcept
    : base_(static_cast<std::byte*>(buffer)), capacity_(capacity), offset_(0) {}

void* ArenaAllocator::allocate(std::size_t bytes, std::size_t alignment) {
    // Align the current offset up, then confirm the aligned request fits before bumping. We
    // work in offsets rather than pointers so the overflow check is plain integer arithmetic:
    // `bytes > capacity_ - aligned` can't itself overflow because aligned <= capacity_ here.
    const std::size_t aligned = align_up(offset_, alignment);
    if (aligned > capacity_ || bytes > capacity_ - aligned) {
        return nullptr;
    }
    offset_ = aligned + bytes;
    return base_ + aligned;
}

void ArenaAllocator::deallocate(void* /*p*/, std::size_t /*bytes*/) noexcept {
    // Intentionally empty: an arena reclaims in bulk via reset().
}

void ArenaAllocator::reset() noexcept {
    offset_ = 0;
}

} // namespace rime::core
