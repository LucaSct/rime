// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <string_view>

#include "rime/core/reflect/type_info.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/world.hpp"

// M8.2: the Destructible ECS component is reflected (so the M9 inspector and M11 replication get it
// for free, per ADR-0018) and registers with a world — the same contract the physics components
// hold.
using namespace rime;

TEST_CASE("M8.2: Destructible reflects its authored field") {
    const core::TypeInfo& info = core::ReflectionTraits<destruction::Destructible>::info();
    REQUIRE(info.fields.size() == 1);
    CHECK(std::string_view(info.fields[0].name) == "asset");
    CHECK(info.type_hash != 0); // a stable schema fingerprint got computed
}

TEST_CASE("M8.2: destruction components register with a world (idempotently)") {
    ecs::World world;
    destruction::register_destruction_components(world);
    // Registration is idempotent (ADR-0018) — calling again must be harmless and id-stable.
    const ecs::ComponentId first = world.register_component<destruction::Destructible>();
    destruction::register_destruction_components(world);
    const ecs::ComponentId again = world.register_component<destruction::Destructible>();
    CHECK(first == again);
}

TEST_CASE("M8.2: an entity can author a Destructible referencing a cooked pattern") {
    ecs::World world;
    destruction::register_destruction_components(world);
    const ecs::Entity e = world.spawn_with(destruction::Destructible{0xABCDEF01});
    const destruction::Destructible* d = world.get<destruction::Destructible>(e);
    REQUIRE(d != nullptr);
    CHECK(d->asset == 0xABCDEF01ull);
}
