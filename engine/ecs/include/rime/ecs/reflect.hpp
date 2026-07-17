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

// Reflection blocks (outside the namespace — the macros open rime::core themselves). Order matters:
// the nested type must be registered before its container. Entity (a handle) and core::Transform
// (via math/reflect.hpp) come first; the wrapper components reference them.
RIME_REFLECT_BEGIN(rime::ecs::Entity)
RIME_REFLECT_FIELD(index)
RIME_REFLECT_FIELD(generation)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::ecs::LocalTransform)
RIME_REFLECT_FIELD(value)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::ecs::WorldTransform)
RIME_REFLECT_FIELD(value)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::ecs::Parent)
RIME_REFLECT_FIELD(value)
RIME_REFLECT_END()
