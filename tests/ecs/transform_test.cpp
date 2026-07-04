// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.5 (the transform hierarchy). propagate_transforms composes each entity's
// WorldTransform down its parent chain (world = parent.world * local; roots: world = local),
// processing the hierarchy depth by depth so a child always sees its parent's finished world. We
// prove roots equal their local, children compose onto their parent, multi-level chains accumulate,
// moving an ancestor moves its descendants, rotation composes (matching core::Transform's
// operator*), and it all holds in parallel at scale — both the flat fast path and a wide hierarchy.
// The parallel paths ride the TSan CI net.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/core/jobs.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"

using namespace rime::ecs;
using rime::core::JobSystem;
using rime::core::Transform;
using rime::core::Vec3;

namespace {
Transform translate(float x, float y, float z) {
    Transform t;
    t.translation = Vec3{x, y, z};
    return t;
}
} // namespace

TEST_CASE("a root's world transform equals its local") {
    World w;
    JobSystem jobs;
    const Entity e = w.spawn_with(LocalTransform{translate(3.0f, 4.0f, 5.0f)}, WorldTransform{});
    propagate_transforms(w, jobs);
    const Vec3 t = w.get<WorldTransform>(e)->value.translation;
    CHECK(t.x == 3.0f);
    CHECK(t.y == 4.0f);
    CHECK(t.z == 5.0f);
}

TEST_CASE("a child composes onto its parent's world") {
    World w;
    JobSystem jobs;
    const Entity parent =
        w.spawn_with(LocalTransform{translate(10.0f, 0.0f, 0.0f)}, WorldTransform{});
    const Entity child =
        w.spawn_with(LocalTransform{translate(1.0f, 0.0f, 0.0f)}, WorldTransform{}, Parent{parent});
    propagate_transforms(w, jobs);
    CHECK(w.get<WorldTransform>(parent)->value.translation.x == 10.0f);
    CHECK(w.get<WorldTransform>(child)->value.translation.x == 11.0f); // 10 + 1
}

TEST_CASE("world transforms compose down a multi-level chain") {
    World w;
    JobSystem jobs;
    const Entity a = w.spawn_with(LocalTransform{translate(10.0f, 0.0f, 0.0f)}, WorldTransform{});
    const Entity b =
        w.spawn_with(LocalTransform{translate(1.0f, 0.0f, 0.0f)}, WorldTransform{}, Parent{a});
    const Entity c =
        w.spawn_with(LocalTransform{translate(1.0f, 0.0f, 0.0f)}, WorldTransform{}, Parent{b});
    propagate_transforms(w, jobs);
    CHECK(w.get<WorldTransform>(a)->value.translation.x == 10.0f);
    CHECK(w.get<WorldTransform>(b)->value.translation.x == 11.0f);
    CHECK(w.get<WorldTransform>(c)->value.translation.x == 12.0f);
}

TEST_CASE("moving an ancestor moves its descendants on the next propagate") {
    World w;
    JobSystem jobs;
    const Entity p = w.spawn_with(LocalTransform{translate(0.0f, 0.0f, 0.0f)}, WorldTransform{});
    const Entity c =
        w.spawn_with(LocalTransform{translate(1.0f, 0.0f, 0.0f)}, WorldTransform{}, Parent{p});
    propagate_transforms(w, jobs);
    CHECK(w.get<WorldTransform>(c)->value.translation.x == 1.0f);

    w.get<LocalTransform>(p)->value.translation.x = 100.0f; // move the parent
    propagate_transforms(w, jobs);
    CHECK(w.get<WorldTransform>(p)->value.translation.x == 100.0f);
    CHECK(w.get<WorldTransform>(c)->value.translation.x == 101.0f); // the child followed
}

TEST_CASE("composition includes rotation (matches core::Transform operator*)") {
    World w;
    JobSystem jobs;
    Transform parent_local;
    parent_local.translation = Vec3{5.0f, 0.0f, 0.0f};
    parent_local.rotation = rime::core::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 1.5707963f);
    const Transform child_local = translate(1.0f, 0.0f, 0.0f);

    const Entity parent = w.spawn_with(LocalTransform{parent_local}, WorldTransform{});
    const Entity child =
        w.spawn_with(LocalTransform{child_local}, WorldTransform{}, Parent{parent});
    propagate_transforms(w, jobs);

    // parent is a root, so parent.world == parent_local; the child's world must be parent_local *
    // child_local (rotate the child's offset by 90° about Z, then translate).
    const Transform expected = parent_local * child_local;
    CHECK(rime::core::approx_eq(w.get<WorldTransform>(child)->value, expected));
}

TEST_CASE("a child whose parent is not a transform node is treated as a root") {
    World w;
    JobSystem jobs;
    const Entity plain = w.spawn(); // no transform components
    const Entity child =
        w.spawn_with(LocalTransform{translate(7.0f, 0.0f, 0.0f)}, WorldTransform{}, Parent{plain});
    propagate_transforms(w, jobs);
    CHECK(w.get<WorldTransform>(child)->value.translation.x == 7.0f); // world = local
}

TEST_CASE("a large flat scene propagates in parallel (fast path)") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 50000;
    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(LocalTransform{translate(static_cast<float>(i), 0.0f, 0.0f)},
                                  WorldTransform{}));
    }
    propagate_transforms(w, jobs);

    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        ok = ok && (w.get<WorldTransform>(es[i])->value.translation.x == static_cast<float>(i));
    }
    CHECK(ok);
}

TEST_CASE("a wide hierarchy propagates correctly in parallel") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kRoots = 4;
    constexpr std::uint32_t kChildrenPerRoot = 12000;

    std::vector<Entity> roots;
    std::vector<Entity> children;
    for (std::uint32_t r = 0; r < kRoots; ++r) {
        const Entity root = w.spawn_with(
            LocalTransform{translate(static_cast<float>(r * 100), 0.0f, 0.0f)}, WorldTransform{});
        roots.push_back(root);
        for (std::uint32_t k = 0; k < kChildrenPerRoot; ++k) {
            children.push_back(w.spawn_with(
                LocalTransform{translate(1.0f, 0.0f, 0.0f)}, WorldTransform{}, Parent{root}));
        }
    }
    propagate_transforms(w, jobs);

    bool ok = true;
    for (std::uint32_t r = 0; r < kRoots; ++r) {
        ok = ok &&
             (w.get<WorldTransform>(roots[r])->value.translation.x == static_cast<float>(r * 100));
    }
    // Every child sits at its root's x + 1.
    for (std::size_t i = 0; i < children.size(); ++i) {
        const std::uint32_t r = static_cast<std::uint32_t>(i / kChildrenPerRoot);
        ok = ok && (w.get<WorldTransform>(children[i])->value.translation.x ==
                    static_cast<float>(r * 100) + 1.0f);
    }
    CHECK(ok);
}
