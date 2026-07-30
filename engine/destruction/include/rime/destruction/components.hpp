// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"

// The destruction-facing ECS components (M8.2): how an entity says "I am a destructible." The same
// discipline as the physics components (components.hpp) — a trivially-copyable, standard-layout POD
// of flat fields, so the M9 inspector and M11 replication get it for free. `Destructible` is
// authored *intent* (which cooked pattern, referenced by its content id); the runtime
// `DestructibleInstanceRef` is transient bookkeeping the bind path adds, kept separate and
// unreflected exactly as RigidBodyHandle is. The full ECS bind system (author a Destructible entity
// → DestructionWorld instance) lands with the m8.6 sample; m8.2 defines the authoring surface and
// drives spawning through the DestructionWorld API directly.
namespace rime::destruction {

// Author this entity as a destructible: `asset` is the AssetId (content-hash) value of the cooked
// Destructible pattern to instance at this entity's transform. 0 = unset.
struct Destructible {
    std::uint64_t asset = 0;
};

// "This entity is not standing as an instance" — the DestructibleInstanceRef null. Also what the
// bind pass (bind.hpp) tests to decide whether an entity still needs standing up.
inline constexpr std::uint32_t kUnboundInstance = 0xFFFFFFFFu;

// The runtime link from an entity to its spawned DestructionWorld instance — the RigidBodyHandle
// analogue. Transient (a fresh bind regenerates it), so deliberately NOT reflected: the inspector
// shows what an entity *is* (Destructible), not the instance-index bookkeeping behind it.
//
// Staying unreflected is load-bearing for m11.4 and not merely tidy: this index is a LOCAL table
// position, and the two peers' tables are unrelated. Were it reflected it would replicate, and a
// client would then be handed the server's instance index for its own — silently naming a different
// wall, or none. The entity's NetId is the shared name; this is each peer's private answer to it.
struct DestructibleInstanceRef {
    std::uint32_t instance = kUnboundInstance; // an InstanceId index; kUnboundInstance = unbound
};

// Register the destruction components with a world — id + size + reflection TypeInfo in one shot.
// Idempotent (World::register_component is); calling it first keeps component ids stable.
inline void register_destruction_components(ecs::World& world) {
    (void)world.register_component<Destructible>();
    (void)world.register_component<DestructibleInstanceRef>();
}

} // namespace rime::destruction

// Reflection (outside the namespace — the macros open rime::core themselves). Only the authored
// intent is reflected; the runtime handle is not (see above).
RIME_REFLECT_BEGIN(rime::destruction::Destructible)
RIME_REFLECT_FIELD(asset)
RIME_REFLECT_END()
