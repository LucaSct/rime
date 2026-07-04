// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.2a (component signature). A signature is the canonical (sorted, deduplicated) set of
// component ids identifying an archetype: construction normalizes order and duplicates, equality
// and hashing are order-independent, with()/without() compute the add/remove-component target, and
// contains_all() is the query superset test.

#include <doctest/doctest.h>

#include <cstdint>
#include <unordered_map>

#include "rime/ecs/signature.hpp"

using namespace rime::ecs;

namespace {
constexpr ComponentId cid(std::uint32_t n) noexcept {
    return static_cast<ComponentId>(n);
}
} // namespace

TEST_CASE("construction sorts and deduplicates; equality is order-independent") {
    const ComponentSignature a{cid(2), cid(0), cid(1), cid(0)};
    CHECK(a.size() == 3); // the duplicate 0 collapsed
    CHECK(a.ids()[0] == cid(0));
    CHECK(a.ids()[1] == cid(1));
    CHECK(a.ids()[2] == cid(2));

    const ComponentSignature b{cid(1), cid(2), cid(0)};
    CHECK(a == b); // same set, different insertion order
    CHECK_FALSE(a != b);
}

TEST_CASE("contains and contains_all") {
    const ComponentSignature s{cid(1), cid(3), cid(5)};
    CHECK(s.contains(cid(3)));
    CHECK_FALSE(s.contains(cid(2)));

    CHECK(s.contains_all(ComponentSignature{cid(1), cid(5)}));
    CHECK(s.contains_all(ComponentSignature{})); // everything contains the empty set
    CHECK(s.contains_all(s));
    CHECK_FALSE(s.contains_all(ComponentSignature{cid(1), cid(2)})); // 2 is missing
}

TEST_CASE("with/without compute the archetype-move target and leave the original unchanged") {
    const ComponentSignature s{cid(1), cid(3)};

    const ComponentSignature added = s.with(cid(2));
    CHECK(added.size() == 3);
    CHECK(added.contains(cid(2)));
    CHECK(added.ids()[1] == cid(2)); // inserted in sorted position
    CHECK(s.with(cid(3)) == s);      // adding a present id is a no-op

    const ComponentSignature removed = s.without(cid(1));
    CHECK(removed.size() == 1);
    CHECK_FALSE(removed.contains(cid(1)));
    CHECK(s.without(cid(9)) == s); // removing an absent id is a no-op

    // The original is untouched by either.
    CHECK(s.size() == 2);
    CHECK(s.contains(cid(1)));
    CHECK(s.contains(cid(3)));
}

TEST_CASE("empty signature") {
    const ComponentSignature empty;
    CHECK(empty.empty());
    CHECK(empty.size() == 0);
    CHECK_FALSE(empty.contains(cid(0)));
}

TEST_CASE("equal signatures hash equal and work as a map key") {
    const ComponentSignature a{cid(4), cid(1), cid(7)};
    const ComponentSignature b{cid(7), cid(4), cid(1)};
    CHECK(a.hash() == b.hash());

    std::unordered_map<ComponentSignature, int> archetypes;
    archetypes[a] = 42;
    REQUIRE(archetypes.find(b) != archetypes.end()); // b keys to the same bucket/entry as a
    CHECK(archetypes[b] == 42);
    CHECK(archetypes.size() == 1);
}
