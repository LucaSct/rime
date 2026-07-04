// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.2a (chunk pool). Blocks are the right size and cache-line aligned, distinct while
// live, and recycled on free; the pool grows a new superblock only when the current ones are full;
// and a block's full 16 KiB is usable without treading on its neighbours (checked here, and under
// ASan in CI). This is core::PoolAllocator carrying real load (ADR-0018).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/ecs/chunk_pool.hpp"

using namespace rime::ecs;

namespace {
[[nodiscard]] bool aligned(const void* p, std::size_t a) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}
} // namespace

TEST_CASE("blocks are aligned, distinct, and counted while live") {
    ChunkPool pool;
    CHECK(pool.live_chunks() == 0);

    void* a = pool.allocate();
    void* b = pool.allocate();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    CHECK(aligned(a, ChunkPool::kChunkAlign));
    CHECK(aligned(b, ChunkPool::kChunkAlign));
    CHECK(pool.live_chunks() == 2);

    pool.deallocate(a);
    CHECK(pool.live_chunks() == 1);
    pool.deallocate(b);
    CHECK(pool.live_chunks() == 0);
}

TEST_CASE("a freed block is recycled rather than growing the pool") {
    ChunkPool pool(/*chunks_per_super=*/4);
    void* first = pool.allocate();
    const std::size_t supers_after_first = pool.superblock_count();
    pool.deallocate(first);

    void* again = pool.allocate();
    CHECK(again == first);                                // the freed block came back
    CHECK(pool.superblock_count() == supers_after_first); // no growth
    pool.deallocate(again);
}

TEST_CASE("the pool grows superblocks only when full") {
    ChunkPool pool(/*chunks_per_super=*/4);
    CHECK(pool.superblock_count() == 0); // nothing allocated yet

    std::vector<void*> blocks;
    for (int i = 0; i < 4; ++i) {
        blocks.push_back(pool.allocate());
    }
    CHECK(pool.superblock_count() == 1); // first four fit in one superblock

    blocks.push_back(pool.allocate()); // the fifth forces a second superblock
    CHECK(pool.superblock_count() == 2);
    CHECK(pool.live_chunks() == 5);

    for (void* b : blocks) {
        pool.deallocate(b);
    }
    CHECK(pool.live_chunks() == 0);
}

TEST_CASE("each block's full extent is writable without corrupting neighbours") {
    ChunkPool pool(/*chunks_per_super=*/2);
    auto* p = static_cast<std::uint8_t*>(pool.allocate());
    auto* q = static_cast<std::uint8_t*>(pool.allocate());

    // Fill both entire blocks with distinct patterns, then verify neither bled into the other.
    for (std::size_t i = 0; i < ChunkPool::kChunkSize; ++i) {
        p[i] = 0xAB;
        q[i] = 0xCD;
    }
    bool p_intact = true;
    bool q_intact = true;
    for (std::size_t i = 0; i < ChunkPool::kChunkSize; ++i) {
        p_intact = p_intact && (p[i] == 0xAB);
        q_intact = q_intact && (q[i] == 0xCD);
    }
    CHECK(p_intact);
    CHECK(q_intact);

    pool.deallocate(p);
    pool.deallocate(q);
}
