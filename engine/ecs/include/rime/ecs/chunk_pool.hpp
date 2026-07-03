// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "rime/core/memory/pool.hpp"

// The ChunkPool hands out the fixed-size memory blocks that archetype storage lives in — this is
// where `core`'s allocators finally become *load-bearing* (ADR-0018). Every block is `kChunkSize`
// bytes (16 KiB, the ADR's grain) aligned to a cache line, so a chunk's SoA columns never straddle
// one badly and two workers processing different chunks never share a line.
//
// Growth model: the pool owns a list of **superblocks**, each a large aligned allocation carved
// into chunk-sized blocks by a `core::PoolAllocator` (the engine's O(1) fixed-block allocator — its
// free-list runs through the free blocks themselves, no side table). `allocate()` serves a block
// from an existing superblock, or adds a new one when they are all full; `deallocate()` finds the
// owning superblock by address range and returns the block to its pool for reuse. So chunk memory
// is bounded, recycled, and never `realloc`'d out from under a live chunk.
//
// Threading: structural operations (which is what allocate/deallocate are) run on the main thread,
// matching the World's contract — no internal locking.
namespace rime::ecs {

class ChunkPool {
public:
    static constexpr std::size_t kChunkSize = 16 * 1024; // one chunk (ADR-0018)
    static constexpr std::size_t kChunkAlign = 64;       // cache-line aligned

    // `chunks_per_super` sets how many chunks each superblock allocation provides (the growth
    // granularity): larger amortizes system allocations, smaller wastes less on a tiny world.
    explicit ChunkPool(std::size_t chunks_per_super = 64);
    ~ChunkPool();

    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;

    // One `kChunkSize`-byte, `kChunkAlign`-aligned block. Grows a new superblock if all are full.
    [[nodiscard]] void* allocate();

    // Return a block previously handed out by allocate(). Undefined for foreign pointers (asserts
    // in checked builds that the block belongs to this pool).
    void deallocate(void* chunk) noexcept;

    [[nodiscard]] std::size_t live_chunks() const noexcept { return live_; }

    [[nodiscard]] std::size_t superblock_count() const noexcept { return supers_.size(); }

private:
    // A superblock: one aligned backing allocation plus the PoolAllocator that carves it into
    // chunks. Held via unique_ptr because PoolAllocator (an Allocator) is deliberately non-movable,
    // and because a superblock owns a raw aligned allocation it frees in its destructor.
    struct Superblock {
        std::byte* memory = nullptr;
        std::size_t bytes = 0;
        core::PoolAllocator pool;

        Superblock(std::byte* mem,
                   std::size_t size,
                   std::size_t chunk_size,
                   std::size_t chunk_align);
        ~Superblock();
        Superblock(const Superblock&) = delete;
        Superblock& operator=(const Superblock&) = delete;

        [[nodiscard]] bool owns(const void* p) const noexcept {
            return p >= memory && p < memory + bytes;
        }
    };

    void add_superblock();

    std::size_t chunks_per_super_;
    std::size_t live_ = 0;
    std::vector<std::unique_ptr<Superblock>> supers_;
};

} // namespace rime::ecs
