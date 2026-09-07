// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/ground/bind.hpp"

#include <cmath>
#include <vector>

#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ground/derive.hpp"
#include "rime/physics/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"

namespace rime::ground {
namespace {

// A scale is "1" if every axis is within a float hair of it. Deliberately strict: the failure this
// guards is a surface authored at scale 2 whose collider is silently half the drawn size, which is
// the class of bug the module exists to end.
[[nodiscard]] bool is_unscaled(const core::Vec3& s) noexcept {
    constexpr float kEps = 1e-4f;
    return std::fabs(s.x - 1.0f) < kEps && std::fabs(s.y - 1.0f) < kEps &&
           std::fabs(s.z - 1.0f) < kEps;
}

} // namespace

BindStats bind_ground(ecs::World& world, physics::PhysicsWorld& physics) {
    BindStats stats;

    struct Pending {
        ecs::Entity entity;
        physics::BodyId body;
    };

    std::vector<Pending> pending;

    world.query<GroundSurface>().for_each([&](ecs::Entity e, GroundSurface& surface) {
        // ALREADY STANDING IN *THIS* WORLD, which is a stronger question than "has a handle", and
        // the editor is what makes the difference load-bearing: pressing Play builds a brand-new
        // `PhysicsWorld`, while the entity keeps the `GroundBody` written against the old one. A
        // BodyId is an index and a generation, so a handle from a destroyed world still answers
        // `is_valid()` — trusting it would skip the bind and leave the new world with no ground at
        // all, which presents as the ball falling through a floor that is plainly drawn.
        const GroundBody* existing = world.get<GroundBody>(e);
        if (existing != nullptr && existing->body.is_valid() && physics.is_alive(existing->body)) {
            return; // already standing — idempotent
        }
        const ecs::WorldTransform* wt = world.get<ecs::WorldTransform>(e);
        const ecs::LocalTransform* lt = world.get<ecs::LocalTransform>(e);
        const core::Transform placement = wt != nullptr   ? wt->value
                                          : lt != nullptr ? lt->value
                                                          : core::Transform{};
        if (!is_unscaled(placement.scale)) {
            ++stats.scaled_refused;
            return;
        }
        const GroundCollider collider = derive_collider(surface);
        physics::BodyDesc d;
        d.motion = physics::MotionType::Static;
        d.shape = collider.shape;
        // The offset is in the surface's frame, so it rotates with the placement.
        d.position = placement.translation + core::rotate(placement.rotation, collider.offset);
        d.orientation = placement.rotation;
        pending.push_back({e, physics.create_body(d)});
    });

    // Stamped in a second pass: `query().for_each` hands out references into the storage the
    // add would reallocate. An entity re-bound into a new world already carries the component, so
    // the stale handle is overwritten rather than duplicated.
    for (const Pending& p : pending) {
        if (GroundBody* existing = world.get<GroundBody>(p.entity); existing != nullptr) {
            existing->body = p.body;
        } else {
            (void)world.add_component(p.entity, GroundBody{p.body});
        }
        ++stats.bound;
    }
    return stats;
}

std::size_t
apply_ground(ecs::World& world, render::MeshRegistry& meshes, render::MaterialId material) {
    struct Pending {
        ecs::Entity entity;
        render::MeshId mesh;
    };

    std::vector<Pending> pending;

    world.query<GroundSurface>().for_each([&](ecs::Entity e, GroundSurface& surface) {
        if (world.get<render::MeshRef>(e) != nullptr) {
            return; // already dressed
        }
        pending.push_back({e, meshes.add(derive_mesh(surface), "ground")});
    });

    for (const Pending& p : pending) {
        (void)world.add_component(p.entity, render::MeshRef{p.mesh});
        // The material is only a FALLBACK. A consumer that already answers "what does this wear"
        // by another route — blockkit's palette does, from the entity's SlabRole — has said so
        // before this runs, and overwriting it here would make the ground the one prop whose look
        // is decided somewhere else from all the others.
        if (world.get<render::MaterialRef>(p.entity) == nullptr) {
            (void)world.add_component(p.entity, render::MaterialRef{material});
        }
    }
    return pending.size();
}

} // namespace rime::ground
