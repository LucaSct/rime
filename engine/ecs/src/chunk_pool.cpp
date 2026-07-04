// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/chunk_pool.hpp"

#include <new>

#include "rime/core/diagnostics/assert.hpp"

namespace rime::ecs {

ChunkPool::Superblock::Superblock(std::byte* mem,
                                  std::size_t size,
                                  std::size_t chunk_size,
                                  std::size_t chunk_align)
    : memory(mem), bytes(size), pool(mem, size, chunk_size, chunk_align) {}

ChunkPool::Superblock::~Superblock() {
    // Paired with the aligned ::operator new in add_superblock(): an over-aligned allocation MUST
    // be freed through the aligned delete form. Deliberately the *unsized* aligned overload — the
    // size parameter of `operator delete(void*, size_t, align_val_t)` is only a hint that lets an
    // allocator skip a size lookup, and Clang before 19 ships with sized deallocation off by
    // default (-fno-sized-deallocation), so libstdc++ never even declares that overload there
    // (first seen on CI's Clang 18 TSan job; local Clang 21 accepted it silently).
    ::operator delete(memory, std::align_val_t{ChunkPool::kChunkAlign});
}

ChunkPool::ChunkPool(std::size_t chunks_per_super)
    : chunks_per_super_(chunks_per_super == 0 ? 1 : chunks_per_super) {}

ChunkPool::~ChunkPool() = default; // superblocks free their backing memory in their destructors

void ChunkPool::add_superblock() {
    const std::size_t bytes = chunks_per_super_ * kChunkSize;
    // Over-aligned allocation so the superblock base — and therefore every kChunkSize-strided block
    // the PoolAllocator carves from it — is cache-line aligned. std::operator new with align_val_t
    // is the portable way to do this (unlike std::aligned_alloc, it exists on MSVC too).
    void* mem = ::operator new(bytes, std::align_val_t{kChunkAlign});
    supers_.push_back(
        std::make_unique<Superblock>(static_cast<std::byte*>(mem), bytes, kChunkSize, kChunkAlign));
}

void* ChunkPool::allocate() {
    // Serve from any superblock that still has a free block. PoolAllocator::allocate() returns
    // nullptr when its superblock is full, so we fall through to the next one, and grow only if all
    // are full.
    for (auto& super : supers_) {
        if (void* block = super->pool.allocate(kChunkSize, kChunkAlign)) {
            ++live_;
            return block;
        }
    }
    add_superblock();
    void* block = supers_.back()->pool.allocate(kChunkSize, kChunkAlign);
    RIME_ASSERT(block != nullptr); // a fresh superblock always has room for at least one chunk
    ++live_;
    return block;
}

void ChunkPool::deallocate(void* chunk) noexcept {
    if (chunk == nullptr) {
        return;
    }
    for (auto& super : supers_) {
        if (super->owns(chunk)) {
            super->pool.deallocate(chunk, kChunkSize);
            --live_;
            return;
        }
    }
    RIME_ASSERT_MSG(false, "ChunkPool::deallocate: pointer does not belong to this pool");
}

} // namespace rime::ecs
