// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/reflect.hpp" // Vec3 must be reflected BEFORE anything nesting it
#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/weapon.hpp"
#include "rime/physics/world.hpp"

// The ECS face of the character controller — deliberately THIN and deliberately not load-bearing.
//
// `step_character` (character.hpp) IS the controller. It takes plain structs and returns a plain
// struct, which is what lets m12.3's headless server loop and m12.4's client-side predictor call it
// directly — hundreds of times per correction, with no world and no entities anywhere in sight.
// This header is the convenience layer for the ordinary case where a character is an entity: pull
// the components out, call the function, write the answer back.
//
// CharacterConfig and CharacterState are used AS the components rather than mirrored into
// ECS-shaped twins. They already satisfy the ADR-0018 storage contract (trivially copyable,
// standard layout), and core::Vec3 is itself reflected (core/math/reflect.hpp), so their fields
// describe correctly as nested structs. Mirroring them would buy nothing and would create the one
// thing this brick most wants to avoid: two places where a character's state is defined, which is
// how m12.4's replay quietly stops restoring everything.
namespace rime::ecs {
class World;
}

namespace rime::gameplay {

// Register the gameplay components with a world — id + size + reflection TypeInfo in one shot, so
// the M9 inspector and M11 replication get them for free. Idempotent (World::register_component
// is), and calling it early keeps component ids stable.
void register_gameplay_components(ecs::World& world);

// Publish a controller-computed pose to the rest of the engine, and stamp it changed.
//
// THIS EXISTS BECAUSE WRITING `WorldTransform` ALONE IS NOT ENOUGH, and the failure is silent. The
// canonical tick order (docs/design/simulation-tick.md) runs gameplay at step 2 and
// `propagate_transforms` at step 3 — and propagate RECOMPUTES `WorldTransform` from
// `LocalTransform` for every entity carrying both. So a controller that writes only WorldTransform
// has its write discarded one step later by a pass doing exactly its job: the character's own
// `CharacterState` marches off correctly while its transform, its kinematic body, and every
// replicated mirror of it stay at the spawn point forever.
//
// It is silent because `CharacterState` is the thing tests naturally assert on and it is right.
// (Measured at m12.4: an avatar whose state had walked to z = −3.29 had a WorldTransform, a
// LocalTransform and a physics body all still reading z = 0. m12.3's proofs were green throughout.)
//
// So the pose is written to BOTH, and `LocalTransform` is the one that makes it survive. The
// mark_changed calls are for the ECS's change-detection consumers (render upload, editor sync,
// replication); `PhysicsSync::push_in` deliberately does NOT rely on them — it compares against the
// pose it last pushed, because a game may legitimately write a transform without stamping it, and a
// flag with a blind spot is exactly the class of bug guardrail 5 exists about.
//
// A PARENTED character is not supported and `LocalTransform` is left alone for one: the controller
// computes a WORLD-space position by construction, and assigning that to a child's local transform
// would place it at the wrong point in its parent's frame. Such an entity keeps the WorldTransform
// write (which propagate will then overwrite), which is the honest failure — visibly wrong rather
// than subtly wrong — and it is why the parent check is a check and not an assumption.
void write_character_pose(ecs::World& world, ecs::Entity player, const core::Vec3& position);

// Advance ONE character entity by one tick. Reads {CharacterConfig, CharacterState} and the
// entity's physics::RigidBodyHandle (for the BodyId to exclude from queries — PhysicsSync's bind is
// what puts it there), calls step_character, and writes the result into the state component AND the
// entity's WorldTransform, stamping both changed.
//
// Writing WorldTransform is the whole handoff, and the reason the controller never touches a body:
// PhysicsSync::push_in reads that transform next, drives the kinematic body to it, and hands the
// solver the velocity the move implied — which is what makes the player PUSH a crate rather than
// teleport through it (physics/sync.hpp). Tick order: gameplay, then propagate_transforms, then
// reconcile/push_in, then step (docs/design/simulation-tick.md).
//
// A no-op for an entity missing any of the three components. That is deliberate rather than
// defensive: substituting a default config or an implied body id would paper over a bind that never
// happened, and the symptom would surface much later as a character that ignores the world.
void step_character_entity(ecs::World& world,
                           const physics::PhysicsWorld& physics,
                           ecs::Entity player,
                           const CharacterInput& input,
                           float dt,
                           StepStats* stats = nullptr);

} // namespace rime::gameplay

// Reflection (outside the namespace — the macros open rime::core themselves). Field lists mirror
// the structs; a mismatch surfaces as a wrong offset in the serializer/round-trip tests.
RIME_REFLECT_BEGIN(rime::gameplay::CharacterConfig)
RIME_REFLECT_FIELD(radius)
RIME_REFLECT_FIELD(half_height)
RIME_REFLECT_FIELD(max_speed)
RIME_REFLECT_FIELD(accel)
RIME_REFLECT_FIELD(air_accel)
RIME_REFLECT_FIELD(gravity)
RIME_REFLECT_FIELD(jump_speed)
RIME_REFLECT_FIELD(max_slope_cos)
RIME_REFLECT_FIELD(step_height)
RIME_REFLECT_FIELD(snap_distance)
RIME_REFLECT_FIELD(skin)
RIME_REFLECT_FIELD(max_slide_iterations)
RIME_REFLECT_FIELD(max_depenetration_per_tick)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::gameplay::CharacterState)
RIME_REFLECT_FIELD(position)
RIME_REFLECT_FIELD(velocity)
RIME_REFLECT_FIELD(grounded)
RIME_REFLECT_FIELD(ground_normal)
RIME_REFLECT_END()

// The weapon pair (m12.3). Reflected on the same terms as the character pair and for the same
// reasons: the inspector and the replicator get them for free, and there is exactly one definition
// of what a weapon remembers. Note the asymmetry that follows from replicating both — WeaponConfig
// is written once and never again, so the version delta ships it on the entity's first tick and
// then costs nothing forever; WeaponState changes on every shot, which is four bytes on the tick a
// trigger is pulled.
RIME_REFLECT_BEGIN(rime::gameplay::WeaponConfig)
RIME_REFLECT_FIELD(range)
RIME_REFLECT_FIELD(damage)
RIME_REFLECT_FIELD(damage_radius)
RIME_REFLECT_FIELD(impulse)
RIME_REFLECT_FIELD(eye_height)
RIME_REFLECT_FIELD(fire_bit)
RIME_REFLECT_FIELD(cooldown_ticks)
RIME_REFLECT_FIELD(automatic)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::gameplay::WeaponState)
RIME_REFLECT_FIELD(cooldown)
RIME_REFLECT_END()
