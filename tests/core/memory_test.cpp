// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.2 allocators: alignment helpers round correctly; the arena bumps and resets;
// the stack frees in LIFO order via markers; the pool hands out and recycles fixed blocks and
// reports exhaustion; the tracking decorator records live/peak bytes and flags leaks; and
// make_in/destroy_in run the constructor and destructor.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "rime/core/memory.hpp"

using namespace rime::core;

TEST_CASE("align_up rounds up to power-of-two boundaries") {
    CHECK(align_up(0, 16) == 0);
    CHECK(align_up(1, 16) == 16);
    CHECK(align_up(16, 16) == 16);
    CHECK(align_up(17, 16) == 32);
    CHECK(align_up(7, 1) == 7); // alignment of 1 is a no-op
    CHECK(align_up(13, 4) == 16);
    CHECK(align_up(64, 64) == 64);
}

TEST_CASE("arena bumps, respects capacity, and resets") {
    alignas(16) std::array<std::byte, 256> buffer{};
    ArenaAllocator arena(buffer.data(), buffer.size());
    CHECK(arena.used() == 0);
    CHECK(arena.capacity() == 256);

    void* a = arena.allocate(32, 16);
    REQUIRE(a != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(a) % 16 == 0);
    CHECK(arena.used() == 32);

    void* b = arena.allocate(10, 16);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 16 == 0);
    CHECK(b != a);

    // A request larger than the space left fails and leaves the arena untouched.
    const std::size_t before = arena.used();
    CHECK(arena.allocate(1000, 16) == nullptr);
    CHECK(arena.used() == before);

    arena.reset();
    CHECK(arena.used() == 0);
}

TEST_CASE("stack allocator frees in LIFO order via markers") {
    alignas(16) std::array<std::byte, 256> buffer{};
    StackAllocator stack(buffer.data(), buffer.size());

    void* a = stack.allocate(16, 16);
    REQUIRE(a != nullptr);
    const StackAllocator::Marker marker = stack.mark();
    void* b = stack.allocate(32, 16);
    void* c = stack.allocate(8, 8);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(stack.used() > marker);

    // Rewinding to the marker frees b and c (everything after it) but keeps a.
    stack.rewind(marker);
    CHECK(stack.used() == marker);

    // The next allocation reuses the freed space, landing exactly where b was.
    void* d = stack.allocate(32, 16);
    CHECK(d == b);

    stack.reset();
    CHECK(stack.used() == 0);
}

TEST_CASE("pool hands out fixed blocks and recycles them") {
    alignas(16) std::array<std::byte, 256> buffer{};
    PoolAllocator pool(buffer.data(), buffer.size(), 32, 16);
    const std::size_t total = pool.capacity_blocks();
    REQUIRE(total >= 4);
    CHECK(pool.free_blocks() == total);

    void* a = pool.allocate(32, 16);
    void* b = pool.allocate(20, 16); // a smaller request still fits in a 32-byte block
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    CHECK(reinterpret_cast<std::uintptr_t>(a) % 16 == 0);
    CHECK(pool.free_blocks() == total - 2);

    // Returning a block makes it available again; LIFO means the next allocate reuses it.
    pool.deallocate(b, 32);
    CHECK(pool.free_blocks() == total - 1);
    void* c = pool.allocate(32, 16);
    CHECK(c == b);

    // A request larger than a block cannot be served.
    CHECK(pool.allocate(64, 16) == nullptr);
}

TEST_CASE("pool returns nullptr when exhausted") {
    alignas(16) std::array<std::byte, 128> buffer{};
    PoolAllocator pool(buffer.data(), buffer.size(), 32, 16);
    const std::size_t total = pool.capacity_blocks();
    for (std::size_t i = 0; i < total; ++i) {
        CHECK(pool.allocate(32, 16) != nullptr);
    }
    CHECK(pool.free_blocks() == 0);
    CHECK(pool.allocate(32, 16) == nullptr);
}

TEST_CASE("tracking allocator records live/peak bytes and detects leaks") {
    alignas(16) std::array<std::byte, 1024> buffer{};
    ArenaAllocator arena(buffer.data(), buffer.size());
    TrackingAllocator tracker(arena);

    void* a = tracker.allocate(100, 16);
    void* b = tracker.allocate(200, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(tracker.stats().live_allocations == 2);
    CHECK(tracker.stats().live_bytes == 300);
    CHECK(tracker.stats().peak_bytes == 300);
    CHECK(tracker.stats().total_allocations == 2);

    tracker.deallocate(a, 100);
    CHECK(tracker.stats().live_allocations == 1);
    CHECK(tracker.stats().live_bytes == 200);
    CHECK(tracker.stats().peak_bytes == 300); // peak is a high-water mark; it never drops

    // b was never freed, so a leak is outstanding (log=false keeps test output clean).
    CHECK(tracker.report_leaks(false));

    tracker.deallocate(b, 200);
    CHECK_FALSE(tracker.report_leaks(false));
}

namespace {
// Counts its live instances so we can prove make_in/destroy_in run the ctor and dtor.
struct Counted {
    static int live;
    int value;

    explicit Counted(int v) : value(v) { ++live; }

    ~Counted() { --live; }
};

int Counted::live = 0;
} // namespace

TEST_CASE("make_in / destroy_in run the constructor and destructor") {
    alignas(16) std::array<std::byte, 256> buffer{};
    ArenaAllocator arena(buffer.data(), buffer.size());

    CHECK(Counted::live == 0);
    Counted* obj = make_in<Counted>(arena, 42);
    REQUIRE(obj != nullptr);
    CHECK(obj->value == 42);
    CHECK(Counted::live == 1);

    destroy_in(arena, obj);
    CHECK(Counted::live == 0);

    destroy_in<Counted>(arena, nullptr); // a null pointer is ignored
    CHECK(Counted::live == 0);
}
