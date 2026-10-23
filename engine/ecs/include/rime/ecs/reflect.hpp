// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/reflect.hpp" // core::Transform reflection (LocalTransform/WorldTransform wrap it)
#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"

// Reflection for the ECS's own placement components — the transform hierarchy (M4.5) — plus the
// `Entity` handle itself, so a component that REFERENCES another entity (Parent) can be described,
// serialized, and remapped generically. Registering these is what lets the M9 scene format store a
// posed, parented world with zero per-component code: a `LocalTransform` becomes a nested
// `core::Transform` (translation/rotation/scale → x/y/z/w floats), and a `Parent` becomes a single
// entity-reference field the scene writer emits as a scene-local id.
//
// Why reflect `Entity` at all: it is `core::Handle<EntityTag>` = { index, generation }. Describing
// it gives the scene format a reflection-native signal for "this field points at another entity" (a
// Struct field whose type_hash equals Entity's), which is exactly what entity-id remapping keys off
// on load — see docs/design/scene-format.md.
namespace rime::ecs {

// Register the authored transform components a scene stores: LocalTransform (the edited placement)
// and Parent (the hierarchy edge). WorldTransform is deliberately absent — it is DERIVED
// (propagate_transforms recomputes `world = parent.world * local` after a load), so persisting it
// would just bake a stale duplicate. Register it yourself if a tool genuinely wants the computed
// placement to round-trip. Idempotent (register_component is), like register_render_components.
inline void register_transform_components(World& world) {
    (void)world.register_component<LocalTransform>();
    (void)world.register_component<Parent>();
}

} // namespace rime::ecs

// The reflection blocks for Entity, LocalTransform and Parent used to live here. They now sit with
// their declarations — ecs/entity.hpp and ecs/transform.hpp — because a COMPONENT's reflection
// cannot safely be optional: World::add_component registers implicitly, through a template, so a
// TU that sees the component without its ReflectionTraits specialization silently produces an
// unreflected duplicate of the same weak symbol. The full argument, and the binary it broke, are
// in the note at the bottom of ecs/transform.hpp.
//
// What stays here is the POLICY above: which of the transform components a scene is expected to
// store. That is a decision, not a description, and a caller that wants a different set simply
// calls register_component itself.
