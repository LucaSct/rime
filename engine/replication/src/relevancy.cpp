// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/relevancy.hpp"

#include <algorithm>
#include <utility>

#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"

namespace rime::replication {
namespace {

// World placement first, local as the fallback — see the header for why insisting on WorldTransform
// would have failed to locate debris, which are the population this policy mainly exists to cull.
bool position_of(const ecs::World& world, ecs::Entity entity, core::Vec3& out) {
    if (const auto* world_transform = world.get<ecs::WorldTransform>(entity)) {
        out = world_transform->value.translation;
        return true;
    }
    if (const auto* local = world.get<ecs::LocalTransform>(entity)) {
        out = local->value.translation;
        return true;
    }
    return false;
}

// Per-client hysteresis memory: "which entities were inside the radius when I last scored for this
// client". Without it, an entity hovering on the boundary re-enters every other tick, and each
// entry costs a forced full-state send plus the loss of the per-chunk delta skip for that whole
// tick.
struct HysteresisState {
    struct PerClient {
        net::SessionId id{};
        // Indexed by `Entity::index`, holding that entity's generation PLUS ONE, so that a
        // zero-initialized slot unambiguously means "nobody", whatever value generations start at.
        //
        // Storing the generation rather than a bare flag is the lesson of instance six in
        // docs/design/replication.md, applied before it could become instance seven: this is a
        // per-item record keyed by a RECYCLABLE index. A bare bit would be inherited by whatever
        // entity next occupies the slot, which would silently grant a brand-new entity the
        // more-generous exit radius it never earned. Comparing the generation makes a recycled slot
        // read as "not inside", which is the truth.
        std::vector<std::uint32_t> inside;
    };

    std::vector<PerClient> clients;

    // Linear scan, matching ServerReplicator::find_client — the client count is small and bounded,
    // and a flat vector keeps the common case in one cache line rather than chasing a hash table.
    std::vector<std::uint32_t>& for_client(net::SessionId id) {
        for (PerClient& client : clients) {
            if (client.id.index != id.index) {
                continue;
            }
            // Same slot, different generation: a previous session died and this one recycled its
            // index. It is a DIFFERENT CLIENT standing somewhere else entirely, so it must not
            // inherit the dead one's idea of what was in range. Same rule as above, one level up.
            if (client.id.generation != id.generation) {
                client.id = id;
                client.inside.clear();
            }
            return client.inside;
        }
        clients.push_back(PerClient{id, {}});
        return clients.back().inside;
    }
};

} // namespace

RelevancyFn distance_relevancy(const ecs::World& world, DistanceRelevancy config) {
    // The state is shared rather than owned so the returned callable stays copyable — `RelevancyFn`
    // is a std::function, and ServerReplicator stores it by value.
    auto state = std::make_shared<HysteresisState>();
    const ecs::World* world_ptr = &world;

    return [world_ptr, config = std::move(config), state](net::SessionId id,
                                                          std::span<const ecs::Entity> candidates,
                                                          std::span<float> priorities) {
        core::Vec3 eye{};
        const bool have_viewpoint =
            static_cast<bool>(config.viewpoint) && config.viewpoint(id, eye);

        // A client with no viewpoint gets everything. It is the state every session is in for its
        // first few ticks, and — until M12 builds a player controller — the state a session may
        // simply stay in. Culling against a viewpoint we do not have would mean culling against the
        // origin, which is not a conservative guess but a wrong one.
        if (!have_viewpoint) {
            std::fill(priorities.begin(), priorities.end(), kUnpositionedPriority);
            return;
        }

        std::vector<std::uint32_t>& inside = state->for_client(id);

        const float enter_radius = std::max(0.0f, config.radius);
        const float exit_radius = enter_radius * (1.0f + std::max(0.0f, config.hysteresis));

        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const ecs::Entity entity = candidates[i];

            core::Vec3 position{};
            if (!position_of(*world_ptr, entity, position)) {
                // Cannot be measured, so it is never culled — failing open. See the header: a
                // distance policy that dropped what it could not locate would quietly stop
                // replicating every non-spatial entity in the game.
                priorities[i] = kUnpositionedPriority;
                continue;
            }

            const bool was_inside =
                entity.index < inside.size() && inside[entity.index] == entity.generation + 1u;
            const float threshold = was_inside ? exit_radius : enter_radius;
            const float distance = core::length(position - eye);

            if (distance > threshold) {
                priorities[i] = 0.0f;
                if (entity.index < inside.size()) {
                    inside[entity.index] = 0;
                }
                continue;
            }

            // Strictly decreasing, 1.0 at the viewpoint, 0.5 at the radius, never zero — the
            // relevance decision was the comparison above, so nothing rides on this reaching 0.
            priorities[i] = enter_radius / (enter_radius + distance);
            if (entity.index >= inside.size()) {
                inside.resize(static_cast<std::size_t>(entity.index) + 1, 0u);
            }
            inside[entity.index] = entity.generation + 1u;
        }
    };
}

} // namespace rime::replication
