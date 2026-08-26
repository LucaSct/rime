// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay/components.hpp"

#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/components.hpp"

// The ECS glue. Everything interesting is in character.cpp; this file exists so that including
// components.hpp does not drag ecs::World's guts into every translation unit that only wants the
// component definitions.
namespace rime::gameplay {

void register_gameplay_components(ecs::World& world) {
    (void)world.register_component<CharacterConfig>();
    (void)world.register_component<CharacterState>();
    (void)world.register_component<WeaponConfig>();
    (void)world.register_component<WeaponState>();
}

void step_character_entity(ecs::World& world,
                           const physics::PhysicsWorld& physics,
                           ecs::Entity player,
                           const CharacterInput& input,
                           float dt,
                           StepStats* stats) {
    const CharacterConfig* config = world.get<CharacterConfig>(player);
    CharacterState* state = world.get<CharacterState>(player);
    const physics::RigidBodyHandle* handle = world.get<physics::RigidBodyHandle>(player);
    if (config == nullptr || state == nullptr || handle == nullptr) {
        return; // not a bound character — see the header on why this is not filled in with defaults
    }

    *state = step_character(*state, input, *config, physics, handle->body, dt, stats);
    world.mark_changed<CharacterState>(player);

    // The handoff to physics and to everything else that reads a transform — see the long note on
    // `write_character_pose`. `push_in` is what turns the pose into motion the solver can see,
    // which is what makes a player PUSH a crate rather than teleport through it (physics/sync.hpp).
    write_character_pose(world, player, state->position);
}

void write_character_pose(ecs::World& world, ecs::Entity player, const core::Vec3& position) {
    // LocalTransform FIRST and unconditionally for a root, because it is the one
    // propagate_transforms reads. Writing only WorldTransform is the silent failure the header
    // documents.
    const ecs::Parent* parent = world.get<ecs::Parent>(player);
    const bool is_root = parent == nullptr || !parent->value.is_valid();
    if (is_root) {
        if (ecs::LocalTransform* local = world.get<ecs::LocalTransform>(player); local != nullptr) {
            local->value.translation = position;
            world.mark_changed<ecs::LocalTransform>(player);
        }
    }

    // WorldTransform too, so a consumer reading it BEFORE propagate runs this tick — push_in, a
    // query, a same-tick trigger — sees the fresh pose rather than last tick's. For a root the two
    // agree, so propagate's recompute is then a no-op rather than a correction.
    if (ecs::WorldTransform* world_tf = world.get<ecs::WorldTransform>(player);
        world_tf != nullptr) {
        world_tf->value.translation = position;
        world.mark_changed<ecs::WorldTransform>(player);
    }
}

} // namespace rime::gameplay
