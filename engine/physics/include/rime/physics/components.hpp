// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"

// The physics-facing ECS components (M7.1): how an entity says "simulate me". Trivially-copyable,
// standard-layout PODs with flat primitive fields (the ADR-0018 storage contract + what reflection
// describes today), so the M9 inspector and M11 replication get them for free. The runtime BodyId
// is NOT here — it rides a separate RigidBodyHandle that the M7.6 bind system adds; these two
// components describe *intent*, and stay orthogonal to the transform (WorldTransform is "where").
//
// `motion` and `shape_type` are uint32 (not the tighter uint8 enums) because reflection describes
// UInt32 but not UInt8; the bind system maps them to MotionType / ShapeType.
namespace rime::physics {

// Simulate this entity as a rigid body. Defaults describe a 1 kg dynamic body.
struct RigidBody {
    std::uint32_t motion = 2; // MotionType::Dynamic
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.05f;
    float gravity_factor = 1.0f;
};

// Its collision shape. Only the fields relevant to `shape_type` are read (Sphere: radius; Box:
// half_x/y/z; Capsule: radius + half_height). `sensor` = a trigger that reports overlaps (M7.8)
// but generates no contact response.
struct Collider {
    std::uint32_t shape_type = 0; // ShapeType::Sphere
    float radius = 0.5f;
    float half_x = 0.5f;
    float half_y = 0.5f;
    float half_z = 0.5f;
    float half_height = 0.5f;
    bool sensor = false;
};

// Register the physics components with a world — id + size + reflection TypeInfo in one shot.
// Idempotent (World::register_component is), and calling it first keeps component ids stable.
inline void register_physics_components(ecs::World& world) {
    (void)world.register_component<RigidBody>();
    (void)world.register_component<Collider>();
}

} // namespace rime::physics

// Reflection (outside the namespace — the macros open rime::core themselves). Field lists mirror
// the structs; a mismatch surfaces as a wrong offset in the serializer/round-trip tests.
RIME_REFLECT_BEGIN(rime::physics::RigidBody)
RIME_REFLECT_FIELD(motion)
RIME_REFLECT_FIELD(mass)
RIME_REFLECT_FIELD(friction)
RIME_REFLECT_FIELD(restitution)
RIME_REFLECT_FIELD(linear_damping)
RIME_REFLECT_FIELD(angular_damping)
RIME_REFLECT_FIELD(gravity_factor)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::physics::Collider)
RIME_REFLECT_FIELD(shape_type)
RIME_REFLECT_FIELD(radius)
RIME_REFLECT_FIELD(half_x)
RIME_REFLECT_FIELD(half_y)
RIME_REFLECT_FIELD(half_z)
RIME_REFLECT_FIELD(half_height)
RIME_REFLECT_FIELD(sensor)
RIME_REFLECT_END()
