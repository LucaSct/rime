// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.1 (entity directory). The load-bearing guarantees: allocate hands out distinct live
// entities; free ends liveness and is idempotent; the generation guard rejects a stale Entity even
// after its index is recycled (the use-after-free defense); location records resolve for live
// entities and vanish for stale ones; and the directory survives clear() and a large spawn/free
// churn with consistent counts.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN // this TU supplies doctest's main() for the exe
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/ecs/entity_directory.hpp"

using namespace rime::ecs;

TEST_CASE("allocate hands out distinct, live entities and tracks the count") {
    EntityDirectory dir;
    CHECK(dir.alive_count() == 0);

    const Entity a = dir.allocate();
    const Entity b = dir.allocate();
    CHECK(a.is_valid());
    CHECK(b.is_valid());
    CHECK(a != b);
    CHECK(dir.is_alive(a));
    CHECK(dir.is_alive(b));
    CHECK(dir.alive_count() == 2);
}

TEST_CASE("a null / garbage entity is rejected without crashing") {
    EntityDirectory dir;
    (void)dir.allocate(); // put something in the table so indices exist

    CHECK_FALSE(dir.is_alive(kNullEntity));
    CHECK(dir.location(kNullEntity) == nullptr);

    const Entity garbage{9999, 3};
    CHECK_FALSE(dir.is_alive(garbage));
    CHECK(dir.location(garbage) == nullptr);
}

TEST_CASE("free ends liveness, decrements the count, and is idempotent") {
    EntityDirectory dir;
    const Entity a = dir.allocate();
    const Entity b = dir.allocate();

    CHECK(dir.free(a));
    CHECK_FALSE(dir.is_alive(a));
    CHECK(dir.alive_count() == 1);
    CHECK(dir.is_alive(b)); // the survivor is untouched

    // Freeing an already-freed entity is a no-op returning false (no double-free, no underflow).
    CHECK_FALSE(dir.free(a));
    CHECK(dir.alive_count() == 1);
}

TEST_CASE("a stale entity is rejected after its index is recycled (the generation guard)") {
    EntityDirectory dir;
    const Entity first = dir.allocate();
    const std::uint32_t reused_index = first.index;
    CHECK(dir.free(first));

    // The next allocate reuses the freed index but with a bumped generation.
    const Entity second = dir.allocate();
    CHECK(second.index == reused_index);          // same physical slot...
    CHECK(second.generation != first.generation); // ...but a new generation

    // The crucial property: the old id does NOT alias the new occupant.
    CHECK_FALSE(dir.is_alive(first));
    CHECK(dir.location(first) == nullptr);
    CHECK(dir.is_alive(second));
    CHECK(dir.location(second) != nullptr);
}

TEST_CASE("location resolves for live entities and is writable; the store round-trips") {
    EntityDirectory dir;
    const Entity e = dir.allocate();

    EntityLocation* loc = dir.location(e);
    REQUIRE(loc != nullptr);
    CHECK(loc->archetype == 0xFFFFFFFFu); // default: "unplaced" (M4.1 leaves it so)

    // The storage layer (M4.3) writes the location; simulate that and read it back.
    loc->archetype = 7;
    loc->chunk = 2;
    loc->row = 42;

    const EntityLocation* rd = dir.location(e);
    REQUIRE(rd != nullptr);
    CHECK(rd->archetype == 7);
    CHECK(rd->chunk == 2);
    CHECK(rd->row == 42);
}

TEST_CASE("clear invalidates every outstanding entity and leaves the directory usable") {
    EntityDirectory dir;
    const Entity a = dir.allocate();
    const Entity b = dir.allocate();
    dir.clear();

    CHECK(dir.alive_count() == 0);
    CHECK_FALSE(dir.is_alive(a));
    CHECK_FALSE(dir.is_alive(b));

    // Still usable; reused slots carry fresh generations, so the pre-clear ids stay stale.
    const Entity c = dir.allocate();
    CHECK(dir.is_alive(c));
    CHECK_FALSE(dir.is_alive(a));
    CHECK(dir.alive_count() == 1);
}

TEST_CASE("indices are recycled: churn does not grow the table unboundedly") {
    EntityDirectory dir;
    dir.reserve(1024);

    // Allocate a batch, then free it; the freed indices must be reused by the next batch rather
    // than appending fresh slots — so slot_count() stays at the high-water mark, not the total ever
    // spawned.
    std::vector<Entity> batch;
    batch.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        batch.push_back(dir.allocate());
    }
    const std::size_t high_water = dir.slot_count();
    CHECK(high_water == 1000);

    for (const Entity e : batch) {
        CHECK(dir.free(e));
    }
    CHECK(dir.alive_count() == 0);

    for (int i = 0; i < 1000; ++i) {
        (void)dir.allocate();
    }
    CHECK(dir.alive_count() == 1000);
    CHECK(dir.slot_count() == high_water); // reused, not grown
}

TEST_CASE("large spawn/free churn keeps counts and liveness consistent") {
    EntityDirectory dir;
    constexpr int kN = 100'000;

    std::vector<Entity> all;
    all.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        all.push_back(dir.allocate());
    }
    CHECK(dir.alive_count() == static_cast<std::size_t>(kN));

    // Free every other entity; the survivors must remain alive, the freed must be stale.
    for (int i = 0; i < kN; i += 2) {
        CHECK(dir.free(all[static_cast<std::size_t>(i)]));
    }
    CHECK(dir.alive_count() == static_cast<std::size_t>(kN / 2));

    bool all_consistent = true;
    for (int i = 0; i < kN; ++i) {
        const bool should_be_alive = (i % 2 == 1);
        if (dir.is_alive(all[static_cast<std::size_t>(i)]) != should_be_alive) {
            all_consistent = false;
            break;
        }
    }
    CHECK(all_consistent);
}
