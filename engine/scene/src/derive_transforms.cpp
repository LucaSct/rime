// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/scene/derive_transforms.hpp"

#include <vector>

#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"

namespace rime::scene {

// A reflection-driven restore (a .rscene load, or m9.7's play-session PlaySession::stop) only ever
// carries LocalTransform — WorldTransform is derived state and deliberately unreflected
// (reflect.hpp), so it cannot ride a snapshot at all. After such a restore, give every
// LocalTransform holder that lacks one a default WorldTransform, then propagate_transforms composes
// the (possibly parented) local chain into it — the same two-step "load then derive" every
// reflection-driven world reconstruction needs. Collect-then-add because add_component relocates an
// entity between archetypes (the "archetype move"), which would invalidate a query mid-iteration.
void derive_world_transforms(ecs::World& world, core::JobSystem& jobs) {
    std::vector<ecs::Entity> posed;
    world.query<ecs::LocalTransform>().for_each([&](ecs::Entity e, ecs::LocalTransform&) {
        if (!world.has<ecs::WorldTransform>(e)) {
            posed.push_back(e);
        }
    });
    for (const ecs::Entity e : posed) {
        (void)world.add_component<ecs::WorldTransform>(e, ecs::WorldTransform{});
    }
    ecs::propagate_transforms(world, jobs);
}

} // namespace rime::scene
