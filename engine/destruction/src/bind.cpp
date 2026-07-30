// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction/bind.hpp"

#include <vector>

#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/physics/world.hpp"

// The bind system (m11.4). See bind.hpp for why this exists and why replication is what finally
// forced it. The implementation is deliberately dull: one query, one spawn per unbound entity, one
// component write. All the design is in the header.
namespace rime::destruction {

namespace {

// Where to stand the instance. WorldTransform is the propagated placement, so it is the right
// answer whenever the destructible is parented; it is DERIVED state though (ecs/reflect.hpp: not
// reflected, recomputed every tick by propagate_transforms), which means a freshly-arrived mirror
// may not have one yet. LocalTransform is what actually replicates, so it is the fallback that
// makes a client bind correctly on the very tick a spawn lands rather than one tick later.
[[nodiscard]] core::Transform placement_of(const ecs::World& world, ecs::Entity e) {
    if (const ecs::WorldTransform* wt = world.get<ecs::WorldTransform>(e)) {
        return wt->value;
    }
    if (const ecs::LocalTransform* lt = world.get<ecs::LocalTransform>(e)) {
        return lt->value;
    }
    return core::Transform{};
}

} // namespace

BindStats bind_destructibles(ecs::World& world,
                             DestructionWorld& destruction,
                             physics::PhysicsWorld& physics,
                             const PatternResolver& resolve,
                             Authority authority) {
    BindStats stats;

    // Collect first, mutate after. `spawn` creates physics bodies and `add_component` relocates the
    // entity between archetypes — a structural change, which query.hpp forbids from inside
    // for_each. Two passes over a handful of entities is the cheap and obviously-correct way to
    // respect that; the alternative (a deferred-command buffer) buys nothing at this size.
    std::vector<std::pair<ecs::Entity, std::uint64_t>> pending;
    world.query<Destructible>().for_each([&](ecs::Entity e, Destructible& d) {
        if (d.asset == 0) {
            return; // authored but not pointed at anything yet — the editor's half-filled state
        }
        if (const DestructibleInstanceRef* ref = world.get<DestructibleInstanceRef>(e)) {
            if (ref->instance != kUnboundInstance) {
                return; // already standing
            }
        }
        pending.emplace_back(e, d.asset);
    });

    for (const auto& [entity, asset] : pending) {
        const PatternId pattern = resolve(asset);
        if (!pattern.is_valid()) {
            ++stats.unresolved;
            continue; // content this peer does not hold — retried next tick, so a destructible
                      // whose pattern is still streaming in simply appears late rather than never
        }
        const InstanceId instance =
            destruction.spawn(pattern, placement_of(world, entity), physics);
        if (!instance.is_valid()) {
            ++stats.unresolved;
            continue; // the pattern resolved but the world refused to stand it (a rejected cook)
        }
        destruction.set_authority(instance, authority);

        if (DestructibleInstanceRef* ref = world.get<DestructibleInstanceRef>(entity)) {
            ref->instance = instance.index;
        } else {
            (void)world.add_component(entity, DestructibleInstanceRef{instance.index});
        }
        ++stats.bound;
    }
    return stats;
}

void build_instance_entity_table(const ecs::World& world, std::vector<ecs::Entity>& out) {
    out.clear();
    // const_cast: query() is a non-const member (it hands out mutable component references), but
    // this pass only reads. Taking a `const World&` is the honest signature for what it does, and
    // paying for it here is better than making every caller hold a mutable world it does not need.
    ecs::World& mutable_world = const_cast<ecs::World&>(world);
    mutable_world.query<DestructibleInstanceRef>().for_each(
        [&out](ecs::Entity e, DestructibleInstanceRef& ref) {
            if (ref.instance == kUnboundInstance) {
                return;
            }
            if (ref.instance >= out.size()) {
                out.resize(static_cast<std::size_t>(ref.instance) + 1, ecs::kNullEntity);
            }
            out[ref.instance] = e;
        });
}

} // namespace rime::destruction
