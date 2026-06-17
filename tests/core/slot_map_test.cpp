// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.5 (slot map / generational handles). The load-bearing guarantees: lookups work
// and survive other mutations; erase frees the element and packs the dense array; a handle to an
// erased element is rejected even after its slot is recycled (the use-after-free guard); and the
// dense iteration visits exactly the live elements. We also check garbage handles are rejected
// gracefully and that destructors run.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "rime/core/containers.hpp"

using namespace rime::core;

namespace {
struct Tag; // phantom type for the handles in these tests
}

TEST_CASE("insert returns a working handle; get round-trips the value") {
    SlotMap<int> map;
    CHECK(map.empty());
    const auto a = map.insert(10);
    const auto b = map.insert(20);
    CHECK(map.size() == 2);
    REQUIRE(map.get(a) != nullptr);
    REQUIRE(map.get(b) != nullptr);
    CHECK(*map.get(a) == 10);
    CHECK(*map.get(b) == 20);
    CHECK(map.contains(a));
}

TEST_CASE("a default / invalid handle is rejected") {
    SlotMap<int> map;
    Handle<int> null_handle;
    CHECK_FALSE(null_handle.is_valid());
    CHECK_FALSE(map.contains(null_handle));
    CHECK(map.get(null_handle) == nullptr);
    // A fabricated out-of-range handle must not crash or alias anything.
    Handle<int> garbage{12345, 7};
    CHECK_FALSE(map.contains(garbage));
    CHECK(map.get(garbage) == nullptr);
}

TEST_CASE("erase removes the element and reports invalidity") {
    SlotMap<int> map;
    const auto a = map.insert(1);
    const auto b = map.insert(2);
    CHECK(map.erase(a));
    CHECK(map.size() == 1);
    CHECK_FALSE(map.contains(a));
    CHECK(map.get(a) == nullptr);
    // The survivor is untouched.
    REQUIRE(map.get(b) != nullptr);
    CHECK(*map.get(b) == 2);
    // Erasing an already-erased handle is a no-op returning false.
    CHECK_FALSE(map.erase(a));
}

TEST_CASE("a stale handle is rejected after the slot is recycled (the generation guard)") {
    SlotMap<int> map;
    const auto first = map.insert(100);
    const std::uint32_t reused_index = first.index;
    CHECK(map.erase(first));

    // The next insert reuses the freed slot index but with a bumped generation.
    const auto second = map.insert(200);
    CHECK(second.index == reused_index);          // same physical slot...
    CHECK(second.generation != first.generation); // ...but a new generation
    // The crucial property: the old handle does NOT alias the new occupant.
    CHECK_FALSE(map.contains(first));
    CHECK(map.get(first) == nullptr);
    CHECK(map.contains(second));
    CHECK(*map.get(second) == 200);
}

TEST_CASE("swap-and-pop erase keeps all other handles valid") {
    SlotMap<int> map;
    std::vector<Handle<int>> handles;
    for (int i = 0; i < 8; ++i) {
        handles.push_back(map.insert(i * 10));
    }
    // Erase a middle element; the last dense element gets swapped into its place internally.
    CHECK(map.erase(handles[3]));
    CHECK(map.size() == 7);
    CHECK_FALSE(map.contains(handles[3]));
    // Every other handle still resolves to its original value despite the internal move.
    for (int i = 0; i < 8; ++i) {
        if (i == 3) {
            continue;
        }
        REQUIRE(map.get(handles[i]) != nullptr);
        CHECK(*map.get(handles[i]) == i * 10);
    }
}

TEST_CASE("dense iteration visits exactly the live elements") {
    SlotMap<int> map;
    const auto a = map.insert(5);
    map.insert(7);
    map.insert(9);
    map.erase(a);

    int sum = 0;
    int count = 0;
    for (int v : map) { // range-for over the packed dense array
        sum += v;
        ++count;
    }
    CHECK(count == 2);
    CHECK(sum == 16); // 7 + 9; the erased 5 is gone

    // for_each hands back a usable handle for each live value.
    int seen = 0;
    map.for_each([&](Handle<int> h, int& v) {
        CHECK(map.contains(h));
        CHECK(*map.get(h) == v);
        ++seen;
    });
    CHECK(seen == 2);
}

TEST_CASE("clear empties the map and invalidates outstanding handles") {
    SlotMap<int> map;
    const auto a = map.insert(1);
    const auto b = map.insert(2);
    map.clear();
    CHECK(map.empty());
    CHECK_FALSE(map.contains(a));
    CHECK_FALSE(map.contains(b));
    // The map is still usable after clear; reused slots carry fresh generations.
    const auto c = map.insert(3);
    CHECK(map.contains(c));
    CHECK_FALSE(map.contains(a));
}

TEST_CASE("works with a non-trivial value type (move semantics, destructors)") {
    SlotMap<std::string> map;
    const auto h = map.emplace("hello");
    REQUIRE(map.get(h) != nullptr);
    CHECK(*map.get(h) == "hello");
    map.insert(std::string("world"));
    CHECK(map.size() == 2);
    map.clear(); // must run std::string destructors without leaking (checked under sanitizers/CI)
    CHECK(map.empty());
}

TEST_CASE("phantom typing keeps handle kinds distinct") {
    // Handle<Tag> and Handle<int> are different types — a compile-time guard that you can't use
    // one map's handle with another. (We assert the runtime shape here; the type safety is the
    // point.)
    Handle<Tag> h;
    CHECK_FALSE(h.is_valid());
    CHECK(h.index == kInvalidSlotIndex);
}
