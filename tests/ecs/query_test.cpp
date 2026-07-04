// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.3 (queries + chunk-wise iteration). A Query<Ts...> visits exactly the entities that
// have all of Ts — across every matching archetype and all their chunks — handing the body mutable
// references it can read and write. Supersets match; the Entity-first body works; count() is right;
// an unregistered or non-matching component yields nothing; and it all holds when the data spans
// many chunks.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"

using namespace rime::ecs;

namespace qt {
struct Pos {
    float x;
    float y;
};

struct Vel {
    float dx;
    float dy;
};

struct Tag {
    std::int32_t v;
};

struct NeverUsed {
    int z;
};
} // namespace qt

TEST_CASE("a query visits only entities that have all requested components") {
    World w;
    const Entity a = w.spawn_with(qt::Pos{1.0f, 1.0f}, qt::Vel{10.0f, 0.0f});
    const Entity b = w.spawn_with(qt::Pos{2.0f, 2.0f}, qt::Vel{20.0f, 0.0f});
    const Entity c = w.spawn_with(qt::Pos{3.0f, 3.0f}); // no Vel — must be skipped

    // Integrate velocity into position — the canonical system body.
    w.query<qt::Pos, qt::Vel>().for_each([](qt::Pos& p, qt::Vel& v) { p.x += v.dx; });

    CHECK(w.get<qt::Pos>(a)->x == 11.0f);
    CHECK(w.get<qt::Pos>(b)->x == 22.0f);
    CHECK(w.get<qt::Pos>(c)->x == 3.0f); // untouched: it has no Vel
}

TEST_CASE("count reflects the matching set; supersets match") {
    World w;
    (void)w.spawn_with(qt::Pos{0.0f, 0.0f}, qt::Vel{0.0f, 0.0f});
    (void)w.spawn_with(qt::Pos{0.0f, 0.0f}, qt::Vel{0.0f, 0.0f}, qt::Tag{7}); // ⊇ {Pos,Vel}
    (void)w.spawn_with(qt::Pos{0.0f, 0.0f});                                  // only Pos

    CHECK(w.query<qt::Pos>().count() == 3);          // all three have Pos
    CHECK(w.query<qt::Pos, qt::Vel>().count() == 2); // two have both
    CHECK(w.query<qt::Tag>().count() == 1);
    CHECK(w.query<qt::Pos, qt::Vel>().empty() == false);
}

TEST_CASE("the Entity-first body form receives the owning entity") {
    World w;
    const Entity a = w.spawn_with(qt::Pos{5.0f, 0.0f});
    const Entity b = w.spawn_with(qt::Pos{6.0f, 0.0f});

    std::vector<Entity> seen;
    w.query<qt::Pos>().for_each([&](Entity e, qt::Pos& p) {
        seen.push_back(e);
        p.y = p.x; // write-through to confirm the reference is live
    });

    CHECK(seen.size() == 2);
    CHECK((seen[0] == a || seen[1] == a));
    CHECK((seen[0] == b || seen[1] == b));
    CHECK(w.get<qt::Pos>(a)->y == 5.0f);
    CHECK(w.get<qt::Pos>(b)->y == 6.0f);
}

TEST_CASE("an unregistered or unmatched component yields an empty query") {
    World w;
    (void)w.spawn_with(qt::Pos{1.0f, 1.0f});

    // NeverUsed is registered nowhere ⇒ no entity can have it ⇒ empty, and the body never runs.
    int calls = 0;
    auto q = w.query<qt::NeverUsed>();
    CHECK(q.count() == 0);
    q.for_each([&](qt::NeverUsed&) { ++calls; });
    CHECK(calls == 0);

    // Registered but no entity has BOTH: a Pos-only and a Vel-only entity, none with {Pos, Vel}.
    (void)w.spawn_with(qt::Vel{0.0f, 0.0f});
    CHECK(w.is_registered<qt::Vel>());
    CHECK(w.query<qt::Pos, qt::Vel>().count() == 0);
}

TEST_CASE("the empty query visits every entity") {
    World w;
    (void)w.spawn();
    (void)w.spawn_with(qt::Pos{0.0f, 0.0f});
    (void)w.spawn_with(qt::Pos{0.0f, 0.0f}, qt::Vel{0.0f, 0.0f});
    CHECK(w.entity_count() == 3);

    std::size_t visited = 0;
    w.query<>().for_each([&](Entity) { ++visited; });
    CHECK(visited == 3);
    CHECK(w.query<>().count() == 3);
}

TEST_CASE("iteration spans multiple chunks and touches every matching entity exactly once") {
    World w;
    constexpr std::uint32_t kN = 5000; // {Pos, Vel} spans several 16 KiB chunks

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(qt::Pos{static_cast<float>(i), 0.0f}, qt::Vel{1.0f, 0.0f}));
    }
    // Also spawn some non-matching entities that must NOT be visited.
    for (std::uint32_t i = 0; i < 100; ++i) {
        (void)w.spawn_with(qt::Tag{static_cast<std::int32_t>(i)});
    }

    std::size_t visited = 0;
    w.query<qt::Pos, qt::Vel>().for_each([&](qt::Pos& p, qt::Vel& v) {
        p.x += v.dx; // each matched Pos.x should go from i to i+1
        ++visited;
    });
    CHECK(visited == kN);
    CHECK(w.query<qt::Pos, qt::Vel>().count() == kN);

    bool all_ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        all_ok = all_ok && w.get<qt::Pos>(es[i])->x == static_cast<float>(i) + 1.0f;
    }
    CHECK(all_ok);
}
