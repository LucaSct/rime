// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/transform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"

namespace rime::ecs {

void propagate_transforms(World& world, core::JobSystem& jobs) {
    // Fast path: a flat scene (nothing has a Parent) is all roots, so world = local for every
    // transform entity — one fully-parallel column scan, no hierarchy bookkeeping. This is the
    // common case and the one the M4.6 "100k in parallel" proof exercises.
    if (!world.is_registered<Parent>() || world.query<Parent>().count() == 0) {
        world.query<LocalTransform, WorldTransform>().par_for_each(
            jobs, [](LocalTransform& local, WorldTransform& out) { out.value = local.value; });
        return;
    }

    // General path. Gather the transform entities (serial), then update them depth by depth.
    std::vector<Entity> entities;
    world.query<LocalTransform, WorldTransform>().for_each(
        [&](Entity e, LocalTransform&, WorldTransform&) { entities.push_back(e); });
    if (entities.empty()) {
        return;
    }

    // The parent of `e` for hierarchy purposes: a live entity that is itself a transform node (has
    // a WorldTransform). Anything else — no Parent, null, dead, or a non-transform parent — makes
    // `e` a root, whose world is just its local.
    const auto parent_of = [&world](Entity e) -> Entity {
        const Parent* p = world.get<Parent>(e);
        if (p == nullptr || p->value == kNullEntity) {
            return kNullEntity;
        }
        const Entity par = p->value;
        return (world.is_alive(par) && world.has<WorldTransform>(par)) ? par : kNullEntity;
    };

    // Depth of each entity (root = 0), memoized so the whole forest costs O(entities). try_emplace
    // stamps a "computing" sentinel before recursing, so a malformed cycle is broken (a re-entered
    // node acts as a root) rather than recursing forever.
    constexpr std::uint32_t kComputing = 0xFFFFFFFFu;
    std::unordered_map<std::uint32_t, std::uint32_t> depth;
    depth.reserve(entities.size() * 2);
    std::function<std::uint32_t(Entity)> depth_of = [&](Entity e) -> std::uint32_t {
        const auto [it, inserted] = depth.try_emplace(e.index, kComputing);
        if (!inserted) {
            return it->second == kComputing ? 0u : it->second;
        }
        const Entity par = parent_of(e);
        const std::uint32_t d = (par == kNullEntity) ? 0u : depth_of(par) + 1u;
        depth[e.index] = d;
        return d;
    };

    std::uint32_t max_depth = 0;
    for (const Entity e : entities) {
        max_depth = std::max(max_depth, depth_of(e));
    }

    // Bucket entities by depth, then update level by level: level d reads level d-1's finished
    // world transforms, and the whole level is independent, so it updates in parallel. The join
    // between parallel_for calls is the barrier that makes a parent's world visible to its
    // children.
    std::vector<std::vector<Entity>> levels(static_cast<std::size_t>(max_depth) + 1);
    for (const Entity e : entities) {
        levels[depth[e.index]].push_back(e);
    }

    for (std::uint32_t d = 0; d <= max_depth; ++d) {
        const std::vector<Entity>& level = levels[d];
        jobs.parallel_for(level.size(), 256, [&, d](std::size_t i) {
            const Entity e = level[i];
            const LocalTransform* local = world.get<LocalTransform>(e);
            WorldTransform* out = world.get<WorldTransform>(e);
            const Entity par = (d == 0) ? kNullEntity : parent_of(e);
            if (par == kNullEntity) {
                out->value = local->value; // root
            } else {
                const WorldTransform* parent_world = world.get<WorldTransform>(par);
                out->value = parent_world->value * local->value; // world = parent.world * local
            }
        });
    }
}

} // namespace rime::ecs
