// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/physics/sync.hpp"

#include <cstddef>
#include <vector>

#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/components.hpp"
#include "rime/physics/shape.hpp"

namespace rime::physics {
namespace {

// Build a physics ShapeDesc from a Collider component (uint32 shape_type → the tighter enum; only
// the fields relevant to the type are read, per shape.hpp).
[[nodiscard]] ShapeDesc shape_from(const Collider& c) noexcept {
    ShapeDesc s;
    s.type = static_cast<ShapeType>(c.shape_type);
    s.radius = c.radius;
    s.half_extents = core::Vec3{c.half_x, c.half_y, c.half_z};
    s.half_height = c.half_height;
    return s;
}

} // namespace

void PhysicsSync::reconcile(ecs::World& world, PhysicsWorld& physics) {
    // ---- Unbind. Walk the roster (the authoritative list — a despawned entity took its
    // RigidBodyHandle with it, so no query could rediscover it) and destroy any body whose entity
    // is gone or has dropped its intent. Compact in place: survivors slide down to index `w`.
    std::size_t w = 0;
    for (std::size_t i = 0; i < bound_.size(); ++i) {
        const Bound& b = bound_[i];
        const bool wanted = world.is_alive(b.entity) && world.has<RigidBody>(b.entity) &&
                            world.has<Collider>(b.entity);
        if (wanted) {
            bound_[w++] = b;
            continue;
        }
        physics.destroy_body(b.body);
        if (world.is_alive(b.entity)) {
            world.remove_component<RigidBodyHandle>(b.entity); // drop the now-stale link
        }
    }
    bound_.resize(w);

    // ---- Bind. Find intent entities (RigidBody + Collider + WorldTransform) with no handle yet.
    // Collect them first: adding the RigidBodyHandle component is a structural change, which the
    // query iteration forbids (it would restructure the archetypes being scanned).
    std::vector<ecs::Entity> to_bind;
    world.query<RigidBody, Collider, ecs::WorldTransform>().for_each(
        [&](ecs::Entity e, RigidBody&, Collider&, ecs::WorldTransform&) {
            if (!world.has<RigidBodyHandle>(e)) {
                to_bind.push_back(e);
            }
        });

    for (const ecs::Entity e : to_bind) {
        const RigidBody& rb = *world.get<RigidBody>(e);
        const Collider& col = *world.get<Collider>(e);
        const ecs::WorldTransform& wt = *world.get<ecs::WorldTransform>(e);

        BodyDesc d;
        d.motion = static_cast<MotionType>(rb.motion);
        d.shape = shape_from(col);
        d.mass = rb.mass;
        d.friction = rb.friction;
        d.restitution = rb.restitution;
        d.linear_damping = rb.linear_damping;
        d.angular_damping = rb.angular_damping;
        d.gravity_factor = rb.gravity_factor;
        d.position = wt.value.translation; // place the body where the entity already is
        d.orientation = wt.value.rotation;

        const BodyId body = physics.create_body(d);
        world.add_component<RigidBodyHandle>(e, RigidBodyHandle{body});
        bound_.push_back(Bound{e, body, d.motion == MotionType::Dynamic});
    }
}

void PhysicsSync::write_back(ecs::World& world, PhysicsWorld& physics) {
    for (const Bound& b : bound_) {
        // Only dynamic bodies move under simulation, and among those only awake ones moved this
        // tick (M7.5). Skipping the rest is the whole point of awake-only write-back: a settled
        // world stamps nothing, so change-tracking consumers do no work for it.
        if (!b.dynamic || physics.is_asleep(b.body)) {
            continue;
        }
        BodyState s;
        if (!physics.get_body_state(b.body, s)) {
            continue; // defensive: the roster only holds live bodies
        }
        ecs::WorldTransform* wt = world.get<ecs::WorldTransform>(b.entity);
        if (wt == nullptr) {
            continue; // entity lost its WorldTransform between reconcile and now — skip
        }
        wt->value.translation = s.position;
        wt->value.rotation = s.orientation; // scale is the game's; physics never touches it
        world.mark_changed<ecs::WorldTransform>(b.entity);

        // m9.7 (editor Play/Stop): mirror the pose into LocalTransform too, for any entity that
        // carries one. v1 PhysicsSync only ever binds ROOT bodies (a parented body's sync is a
        // documented seam this bridge does not fill yet — the class comment above), so
        // local == world already holds for every body it can move; this just keeps that identity
        // true in BOTH components instead of only WorldTransform. Two things ride on it:
        //   (1) WorldTransform is derived state and deliberately UNREFLECTED (reflect.hpp) — it
        //       cannot appear in a reflection-driven snapshot at all — so a physics body's
        //       placement had no component-level source of truth an editor Play/Stop restore could
        //       recover. LocalTransform IS reflected; mirroring into it is what makes
        //       "reconstructible from components" (ADR-0031 §4) literally true for physics-owned
        //       placement.
        //   (2) It fixes a real clobbering bug the m9.7 restore proof caught:
        //   ecs::propagate_transforms
        //       unconditionally recomposes WorldTransform = LocalTransform for any entity that has
        //       a LocalTransform, every tick, BEFORE this write-back runs. While a body is awake
        //       that was harmless (write-back overwrote the reset every tick); the instant a body
        //       SLEPT, write-back stopped touching it (M7.5's "a settled world writes back
        //       nothing"), but propagate_transforms kept resetting WorldTransform to the STALE
        //       LocalTransform every tick after — a settled body silently snapped back to its spawn
        //       pose. Keeping LocalTransform current makes that reset a no-op.
        if (ecs::LocalTransform* lt = world.get<ecs::LocalTransform>(b.entity)) {
            lt->value = wt->value;
            world.mark_changed<ecs::LocalTransform>(b.entity);
        }
    }
}

void PhysicsSync::step(ecs::World& world, PhysicsWorld& physics, float dt) {
    reconcile(world, physics);
    physics.step(dt);
    write_back(world, physics);
}

} // namespace rime::physics
