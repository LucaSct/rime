// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/containers/handle.hpp"

// An Entity is the ECS's name for a thing in the world — and it is nothing but an id. Following
// ADR-0018, that id is a generational handle (core/containers/handle.hpp): a 32-bit directory
// `index` plus a 32-bit `generation` stamp. The index addresses a slot in the World's entity
// directory (entity_directory.hpp); the generation detects use-after-free — when an entity is
// despawned its slot's generation is bumped, so a stale Entity holding the old generation is
// rejected rather than silently naming whatever entity later reused that index.
//
// Entities carry NO data of their own — that is what Components are, stored column-wise in
// archetypes (M4.2). Reusing core::Handle here is deliberate: the engine already proved and tested
// this exact generational-id machinery (the slot map, resource handles), and phantom-typing on
// EntityTag makes an Entity a distinct type from, say, a Handle<Mesh> at zero runtime cost.
namespace rime::ecs {

// Phantom tag so Entity is its own type in the handle family (it can't be crossed with other
// handle kinds). It is never instantiated — only its identity matters to the type system.
struct EntityTag {};

// The entity id: an 8-byte, trivially-copyable generational handle. Pass it around by value.
using Entity = core::Handle<EntityTag>;

// The null entity: a default handle, structurally invalid (index == core::kInvalidSlotIndex). It
// never names a live entity. Both `e == kNullEntity` and `!e.is_valid()` test for it.
inline constexpr Entity kNullEntity{};

} // namespace rime::ecs
