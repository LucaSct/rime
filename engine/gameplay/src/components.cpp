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

    // The handoff to physics. The controller owns the character's pose; writing it into
    // WorldTransform is how it says so, and push_in is what turns that statement into motion the
    // solver can see. mark_changed is for the ECS's change-detection consumers (render upload,
    // editor sync, replication); push_in deliberately does NOT rely on it — it compares against the
    // pose it last pushed, because a game may legitimately write a transform without stamping it
    // and a flag with a blind spot is exactly the class of bug guardrail 5 exists about.
    if (ecs::WorldTransform* tf = world.get<ecs::WorldTransform>(player); tf != nullptr) {
        tf->value.translation = state->position;
        world.mark_changed<ecs::WorldTransform>(player);
    }
}

} // namespace rime::gameplay
