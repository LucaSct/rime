// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.2b (World ⇄ archetype storage). Entities carry components: add/remove component is
// an archetype move that PRESERVES the components an entity keeps, constructs the added one, drops
// the removed one, and — critically — fixes up the directory location of whatever entity is swapped
// into a vacated slot. get/has resolve through the directory; despawn tears a row out correctly;
// and it all holds at scale. Memory correctness is also covered under ASan.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/ecs/world.hpp"

using namespace rime::ecs;

namespace wc {
struct Pos {
    float x;
    float y;
    float z;
};

struct Vel {
    float dx;
    float dy;
    float dz;
};

struct Health {
    std::int32_t hp;
};
} // namespace wc

TEST_CASE("a fresh entity has no components") {
    World w;
    const Entity e = w.spawn();
    CHECK(w.is_alive(e));
    CHECK_FALSE(w.has<wc::Pos>(e));
    CHECK(w.get<wc::Pos>(e) == nullptr);
    CHECK(w.signature_of(e).empty()); // lives in the empty archetype
}

TEST_CASE("add_component moves the entity to a new archetype and stores the value") {
    World w;
    const Entity e = w.spawn();

    wc::Pos* p = w.add_component<wc::Pos>(e, wc::Pos{1.0f, 2.0f, 3.0f});
    REQUIRE(p != nullptr);
    CHECK(w.has<wc::Pos>(e));
    CHECK(w.get<wc::Pos>(e)->x == 1.0f);
    CHECK(w.get<wc::Pos>(e)->z == 3.0f);
    CHECK(w.signature_of(e).contains(w.component_id<wc::Pos>()));

    // A second component: the first must survive the move to the {Pos, Vel} archetype.
    w.add_component<wc::Vel>(e, wc::Vel{4.0f, 5.0f, 6.0f});
    CHECK(w.get<wc::Pos>(e)->x == 1.0f); // preserved across the archetype move
    CHECK(w.get<wc::Vel>(e)->dy == 5.0f);
    CHECK(w.signature_of(e).size() == 2);
}

TEST_CASE("adding a present component overwrites in place without a move") {
    World w;
    const Entity e = w.spawn();
    w.add_component<wc::Pos>(e, wc::Pos{1.0f, 1.0f, 1.0f});
    const std::size_t archetypes_before = w.archetype_count();

    w.add_component<wc::Pos>(e, wc::Pos{9.0f, 9.0f, 9.0f}); // same component, new value
    CHECK(w.get<wc::Pos>(e)->x == 9.0f);
    CHECK(w.archetype_count() == archetypes_before); // no new archetype created
}

TEST_CASE("remove_component drops the component and keeps the rest") {
    World w;
    const Entity e = w.spawn();
    w.add_component<wc::Pos>(e, wc::Pos{1.0f, 2.0f, 3.0f});
    w.add_component<wc::Vel>(e, wc::Vel{4.0f, 5.0f, 6.0f});

    CHECK(w.remove_component<wc::Pos>(e));
    CHECK_FALSE(w.has<wc::Pos>(e));
    CHECK(w.has<wc::Vel>(e));
    CHECK(w.get<wc::Vel>(e)->dx == 4.0f); // Vel preserved through the move

    CHECK_FALSE(w.remove_component<wc::Pos>(e)); // already gone
    CHECK_FALSE(
        w.remove_component<wc::Health>(e)); // never had it (and unregistered) → false, no crash
}

TEST_CASE("an archetype move fixes up the entity swapped into the vacated slot") {
    World w;
    const Entity a = w.spawn();
    const Entity b = w.spawn();
    const Entity c = w.spawn();
    w.add_component<wc::Pos>(a, wc::Pos{1.0f, 0.0f, 0.0f});
    w.add_component<wc::Pos>(b, wc::Pos{2.0f, 0.0f, 0.0f});
    w.add_component<wc::Pos>(c, wc::Pos{3.0f, 0.0f, 0.0f});
    // a, b, c now share the {Pos} archetype (c is the global-last).

    // Moving b out (add Vel) vacates b's {Pos} slot; c swaps down into it. c's directory location
    // MUST be fixed or the next line reads garbage.
    w.add_component<wc::Vel>(b, wc::Vel{9.0f, 9.0f, 9.0f});

    CHECK(w.get<wc::Pos>(a)->x == 1.0f);
    CHECK(w.get<wc::Pos>(b)->x == 2.0f); // b, now in {Pos, Vel}, kept its Pos
    CHECK(w.get<wc::Vel>(b)->dx == 9.0f);
    CHECK(w.get<wc::Pos>(c)->x == 3.0f); // c was swapped within {Pos} — location fixed up
    CHECK_FALSE(w.has<wc::Vel>(a));
    CHECK_FALSE(w.has<wc::Vel>(c));
}

TEST_CASE("spawn_with builds an entity with components in one call") {
    World w;
    const Entity e = w.spawn_with(wc::Pos{7.0f, 8.0f, 9.0f}, wc::Health{42});
    CHECK(w.has<wc::Pos>(e));
    CHECK(w.has<wc::Health>(e));
    CHECK(w.get<wc::Pos>(e)->y == 8.0f);
    CHECK(w.get<wc::Health>(e)->hp == 42);
}

TEST_CASE("despawn removes the row and fixes up the swapped entity") {
    World w;
    const Entity a = w.spawn_with(wc::Pos{1.0f, 0.0f, 0.0f});
    const Entity b = w.spawn_with(wc::Pos{2.0f, 0.0f, 0.0f});
    CHECK(w.entity_count() == 2);

    CHECK(w.despawn(a)); // b (global-last of {Pos}) swaps into a's slot
    CHECK_FALSE(w.is_alive(a));
    CHECK(w.get<wc::Pos>(a) == nullptr);
    CHECK(w.is_alive(b));
    CHECK(w.get<wc::Pos>(b)->x == 2.0f); // b still readable after the swap
    CHECK(w.entity_count() == 1);
}

TEST_CASE("operations on a stale entity are safe no-ops") {
    World w;
    const Entity e = w.spawn_with(wc::Pos{1.0f, 2.0f, 3.0f});
    w.despawn(e);

    CHECK_FALSE(w.is_alive(e));
    CHECK(w.get<wc::Pos>(e) == nullptr);
    CHECK(w.add_component<wc::Vel>(e, wc::Vel{}) == nullptr);
    CHECK_FALSE(w.remove_component<wc::Pos>(e));
    CHECK_FALSE(w.has<wc::Pos>(e));
    CHECK_FALSE(w.despawn(e)); // already gone
}

TEST_CASE("many entities across chunks: values survive moves and selective despawn") {
    World w;
    constexpr std::uint32_t kN = 4000; // spans several 16 KiB chunks in {Pos, Vel}

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        const Entity e = w.spawn_with(wc::Pos{static_cast<float>(i), 0.0f, 0.0f},
                                      wc::Vel{static_cast<float>(i) * 2.0f, 0.0f, 0.0f});
        es.push_back(e);
    }
    CHECK(w.entity_count() == kN);

    // Every entity reads back the value keyed to its index.
    bool all_ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        const wc::Pos* p = w.get<wc::Pos>(es[i]);
        const wc::Vel* v = w.get<wc::Vel>(es[i]);
        all_ok = all_ok && p != nullptr && v != nullptr && p->x == static_cast<float>(i) &&
                 v->dx == static_cast<float>(i) * 2.0f;
    }
    CHECK(all_ok);

    // Despawn every third entity; the survivors must still resolve to their own values despite all
    // the swap-fixups that despawn triggers.
    for (std::uint32_t i = 0; i < kN; i += 3) {
        w.despawn(es[i]);
    }
    bool survivors_ok = true;
    std::size_t survivors = 0;
    for (std::uint32_t i = 0; i < kN; ++i) {
        if (i % 3 == 0) {
            survivors_ok = survivors_ok && !w.is_alive(es[i]);
            continue;
        }
        ++survivors;
        const wc::Pos* p = w.get<wc::Pos>(es[i]);
        survivors_ok = survivors_ok && p != nullptr && p->x == static_cast<float>(i);
    }
    CHECK(survivors_ok);
    CHECK(w.entity_count() == survivors);
}
