// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.1 (component registry). REGISTER: types get dense ids in registration order, and
// registering twice is idempotent. DESCRIBE: a reflected component captures its TypeInfo (name +
// fields) so it is serializable/inspectable, while an unreflected one still registers with size,
// alignment, and a fallback name. OPERATE: the type-erased ComponentOps build, relocate, and
// destroy raw storage correctly — the machinery the archetype chunks (M4.2) will call. Design:
// ADR-0018.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "rime/core/reflect.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/world.hpp"

// Component test types at file scope (offsetof/reflection want standard-layout, non-anonymous
// types).
namespace ct {
struct Position {
    float x;
    float y;
    float z;
};

struct Velocity { // deliberately NOT reflected
    float vx;
    float vy;
    float vz;
};

struct Frozen {}; // a tag component: no data
} // namespace ct

// Position opts into reflection; Velocity and Frozen do not.
RIME_REFLECT_BEGIN(ct::Position)
RIME_REFLECT_FIELD(x)
RIME_REFLECT_FIELD(y)
RIME_REFLECT_FIELD(z)
RIME_REFLECT_END()

using namespace rime::ecs;

TEST_CASE("registration assigns dense ids in order and is idempotent") {
    ComponentRegistry reg;
    CHECK(reg.count() == 0);

    const ComponentId pos = reg.register_component<ct::Position>();
    const ComponentId vel = reg.register_component<ct::Velocity>();
    CHECK(static_cast<std::uint32_t>(pos) == 0);
    CHECK(static_cast<std::uint32_t>(vel) == 1);
    CHECK(reg.count() == 2);

    // Re-registering returns the same id and does not grow the registry.
    CHECK(reg.register_component<ct::Position>() == pos);
    CHECK(reg.count() == 2);

    CHECK(reg.is_registered<ct::Position>());
    CHECK_FALSE(reg.is_registered<ct::Frozen>());
    CHECK(reg.id_of<ct::Position>() == pos);
}

TEST_CASE("a reflected component captures its TypeInfo (name + fields)") {
    ComponentRegistry reg;
    const ComponentInfo& info = reg.info(reg.register_component<ct::Position>());
    REQUIRE(info.type_info != nullptr);
    CHECK(std::string(info.name) == "ct::Position");
    CHECK(info.type_info->fields.size() == 3);
    CHECK(info.size == sizeof(ct::Position));
    CHECK(info.alignment == alignof(ct::Position));
}

TEST_CASE("an unreflected component still registers (no TypeInfo, fallback name)") {
    ComponentRegistry reg;
    const ComponentInfo& info = reg.info(reg.register_component<ct::Velocity>());
    CHECK(info.type_info == nullptr);
    CHECK(std::string(info.name) == "<unreflected component>");
    CHECK(info.size == sizeof(ct::Velocity));
}

TEST_CASE("a tag (empty) component is a valid component type") {
    ComponentRegistry reg;
    const ComponentInfo& info = reg.info(reg.register_component<ct::Frozen>());
    CHECK(info.size == sizeof(ct::Frozen)); // 1 — C++ has no zero-size complete objects
    CHECK(info.type_info == nullptr);
}

TEST_CASE("component ops build, relocate, and destroy raw storage") {
    ComponentRegistry reg;
    const ComponentInfo& info = reg.info(reg.register_component<ct::Position>());

    constexpr std::size_t kN = 3;
    alignas(ct::Position) std::byte src[sizeof(ct::Position) * kN];
    alignas(ct::Position) std::byte dst[sizeof(ct::Position) * kN];

    // default_construct value-initializes each slot (zeros), then we fill them.
    auto* sp = reinterpret_cast<ct::Position*>(src);
    for (std::size_t i = 0; i < kN; ++i) {
        info.ops.default_construct(src + i * info.size);
        CHECK(sp[i].x == 0.0f); // value-initialized to zero
        sp[i].x = static_cast<float>(i);
        sp[i].y = static_cast<float>(i) + 0.5f;
        sp[i].z = -static_cast<float>(i);
    }

    // relocate moves the whole run to fresh storage (memcpy for trivially-copyable components).
    info.ops.relocate(dst, src, kN);
    auto* dp = reinterpret_cast<ct::Position*>(dst);
    for (std::size_t i = 0; i < kN; ++i) {
        CHECK(dp[i].x == static_cast<float>(i));
        CHECK(dp[i].y == static_cast<float>(i) + 0.5f);
        CHECK(dp[i].z == -static_cast<float>(i));
    }

    info.ops.destroy(dst, kN); // trivially destructible → no-op, but must be callable and safe
}

TEST_CASE("World registers components and returns stable ids through the facade") {
    World world;
    const ComponentId a = world.register_component<ct::Position>();
    const ComponentId b = world.register_component<ct::Velocity>();
    CHECK(a != b);
    CHECK(world.is_registered<ct::Position>());
    CHECK(world.component_id<ct::Position>() == a);
    CHECK(world.registered_component_count() == 2);
}
