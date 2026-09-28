// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for `rime::scene::derive_world_transforms` — the "load then derive" step a
// reflection-driven world reconstruction needs. A `.rscene` load carries LocalTransform only
// (WorldTransform is derived and deliberately unreflected, so it cannot ride a snapshot), and
// `ecs::propagate_transforms` only recomposes entities that already carry BOTH components. So a
// freshly loaded scene has no world poses at all unless someone first adds the missing
// WorldTransform. The claims:
//   (1) a world built with LocalTransform only, and a parent/child pair, ends up with correct
//       composed WorldTransforms after one call — the property that fails if the "add the missing
//       component" step is removed, since propagate then has nothing to recompose;
//   (2) an entity that already has a WorldTransform is not disturbed (its value is left untouched,
//       and derive never invents a LocalTransform for it);
//   (3) an empty world is a no-op.

#include <doctest/doctest.h>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/scene/derive_transforms.hpp"

using namespace rime;

namespace {

core::Transform placed(float tx, float ty, float tz) {
    return core::Transform{
        core::Vec3{tx, ty, tz}, core::quat_identity(), core::Vec3{1.0f, 1.0f, 1.0f}};
}

} // namespace

TEST_CASE("scene: derive_world_transforms composes a LocalTransform-only parent/child") {
    ecs::World w;
    ecs::register_transform_components(w); // LocalTransform + Parent; WorldTransform NOT registered

    // A parent at x=2 and a child at local x=3, parented to it. Neither carries a WorldTransform,
    // exactly like a freshly loaded .rscene: the authored placement is there, the world pose is
    // not.
    const ecs::Entity parent = w.spawn_with(ecs::LocalTransform{placed(2.0f, 0.0f, 0.0f)});
    const ecs::Entity child =
        w.spawn_with(ecs::LocalTransform{placed(3.0f, 0.0f, 0.0f)}, ecs::Parent{parent});

    core::JobSystem jobs;
    scene::derive_world_transforms(w, jobs);

    // If the "add a default WorldTransform to each posed entity" step were removed, neither entity
    // would have a WorldTransform at all (propagate only visits entities that already have both),
    // so these REQUIREs would fail — which is exactly the regression this case exists to catch.
    const ecs::WorldTransform* parent_wt = w.get<ecs::WorldTransform>(parent);
    REQUIRE(parent_wt != nullptr);
    CHECK(parent_wt->value.translation.x == 2.0f); // root: world == local

    const ecs::WorldTransform* child_wt = w.get<ecs::WorldTransform>(child);
    REQUIRE(child_wt != nullptr);
    CHECK(child_wt->value.translation.x == 5.0f); // world = parent.world * local = 2 + 3
}

TEST_CASE("scene: derive_world_transforms leaves a pre-existing WorldTransform alone") {
    ecs::World w;
    ecs::register_transform_components(w);

    // An entity that already carries a WorldTransform (and no LocalTransform) is outside the "has
    // LocalTransform but lacks WorldTransform" set, so derive must leave its value bit-for-bit
    // untouched — and it must not invent a LocalTransform just to have something to derive.
    const ecs::Entity e = w.spawn_with(ecs::WorldTransform{placed(9.0f, 8.0f, 7.0f)});

    core::JobSystem jobs;
    scene::derive_world_transforms(w, jobs);

    const ecs::WorldTransform* wt = w.get<ecs::WorldTransform>(e);
    REQUIRE(wt != nullptr);
    CHECK(wt->value.translation.x == 9.0f);
    CHECK(wt->value.translation.y == 8.0f);
    CHECK(wt->value.translation.z == 7.0f);
    CHECK(w.get<ecs::LocalTransform>(e) == nullptr);
}

TEST_CASE("scene: derive_world_transforms is a no-op on an empty world") {
    ecs::World w;
    ecs::register_transform_components(w);

    core::JobSystem jobs;
    scene::derive_world_transforms(w, jobs); // must not crash on a world with no entities

    CHECK(w.entity_count() == 0);
}
