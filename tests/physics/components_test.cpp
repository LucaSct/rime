// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <string_view>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/components.hpp"

// M7.1: the RigidBody/Collider components are reflected (so the M9 inspector and M11 replication
// get them for free, per ADR-0018) and register with a world. Reflection describing the right
// fields in the right order is what makes the serialized/replicated schema correct.
using namespace rime;

TEST_CASE("RigidBody reflects its fields in declared order") {
    const core::TypeInfo& info = core::ReflectionTraits<physics::RigidBody>::info();
    REQUIRE(info.fields.size() == 7);
    CHECK(std::string_view(info.fields[0].name) == "motion");
    CHECK(std::string_view(info.fields[1].name) == "mass");
    CHECK(std::string_view(info.fields[6].name) == "gravity_factor");
    CHECK(info.type_hash != 0); // a stable schema fingerprint got computed
}

TEST_CASE("Collider reflects its fields in declared order") {
    const core::TypeInfo& info = core::ReflectionTraits<physics::Collider>::info();
    REQUIRE(info.fields.size() == 7);
    CHECK(std::string_view(info.fields[0].name) == "shape_type");
    CHECK(std::string_view(info.fields.back().name) == "sensor");
}

TEST_CASE("physics components register with a world (idempotently)") {
    ecs::World world;
    physics::register_physics_components(world);
    // Registration is idempotent (ADR-0018) — calling again must be harmless and id-stable.
    const ecs::ComponentId rb_first = world.register_component<physics::RigidBody>();
    physics::register_physics_components(world);
    const ecs::ComponentId rb_again = world.register_component<physics::RigidBody>();
    CHECK(rb_first == rb_again);
}
