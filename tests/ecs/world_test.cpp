// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.1 (the World front door, entity side). The World composes the entity directory into
// the public spawn/despawn/is_alive/entity_count API; these checks confirm the lifecycle, the
// generation guard survives the facade, and recycling works through it.

#include <doctest/doctest.h>

#include <vector>

#include "rime/ecs/world.hpp"

using namespace rime::ecs;

TEST_CASE("spawn/despawn drive is_alive and entity_count through the World") {
    World world;
    CHECK(world.entity_count() == 0);

    const Entity e = world.spawn();
    CHECK(world.is_alive(e));
    CHECK(world.entity_count() == 1);

    CHECK(world.despawn(e));
    CHECK_FALSE(world.is_alive(e));
    CHECK(world.entity_count() == 0);
    CHECK_FALSE(world.despawn(e)); // already gone
}

TEST_CASE("a despawned entity stays stale even after its index is reused") {
    World world;
    const Entity a = world.spawn();
    world.despawn(a);
    const Entity b = world.spawn(); // likely reuses a's index with a new generation

    CHECK_FALSE(world.is_alive(a));
    CHECK(world.is_alive(b));
    CHECK(a != b);
}

TEST_CASE("many entities: spawn, selective despawn, and counts") {
    World world;
    world.reserve_entities(10'000);

    std::vector<Entity> es;
    es.reserve(10'000);
    for (int i = 0; i < 10'000; ++i) {
        es.push_back(world.spawn());
    }
    CHECK(world.entity_count() == 10'000);

    for (int i = 0; i < 10'000; i += 3) {
        world.despawn(es[static_cast<std::size_t>(i)]);
    }
    // 10000 indices 0..9999; multiples of 3 in [0,9999] = 3334 of them.
    CHECK(world.entity_count() == 10'000 - 3334);
}
