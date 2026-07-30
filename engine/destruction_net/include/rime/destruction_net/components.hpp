// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"

// The debris↔entity bridge (m11.4b) — how a chunk of rubble becomes something m11.3 can replicate.
//
// THE PROBLEM. `engine/destruction` owns its physics bodies directly rather than through the ECS
// (ADR-0029 §6: `ecs::Collider` cannot name a hull or compound id), so debris are rows in a roster,
// not entities. m11.3 replicates ECS component state and nothing else. Something has to join them.
//
// WHAT DETERMINISM ALREADY GIVES US, AND WHAT IT DOES NOT. Both peers apply the same canonical op
// list at the same fracture boundaries (m11.4a, ADR-0033 A12), and the fracture path is a pure
// function of the alive bits and the cooked bond graph. So both peers DERIVE the same debris set,
// in the same creation order, with the same initial transforms and impulses — m11.4a's proof
// asserts exactly that. Debris IDENTITY and COMPOSITION are therefore free, and are deliberately
// never sent.
//
// What determinism does not give us is the TRAJECTORY afterwards. The two peers are not running
// lockstep: their physics worlds hold different body populations (a client under m11.5's relevancy
// holds a subset), and same-binary determinism is not cross-platform determinism. So the rubble
// drifts. Transforms are replicated; composition is not. That split is exactly what the ROADMAP's
// "destruction events are never culled, debris transforms are distance-budgeted per client"
// presupposes — you can budget a correction, you can never budget an event.
//
// THE ASSOCIATION, AND WHY IT NEEDS NO WIRE FORMAT OF ITS OWN. The one thing that must cross is
// "which of your locally-derived chunks is this transform about". Putting that in a REFLECTED
// component means it rides m11.3's ordinary snapshot path — no new message, no new tag, no new
// completeness rule to get wrong. `DebrisOrigin` names the chunk the way both peers can: the source
// destructible's **NetId**, plus the chunk's ordinal among that instance's debris in creation
// order.
namespace rime::destruction_net {

// Which chunk this entity is. Replicated, so both fields are wire-shared identities: the source
// destructible's NetId spelled out as its two halves (deliberately the raw numbers rather than a
// `replication::NetId` field — this is data on a wire, and the type would suggest a local handle),
// and the ordinal among that instance's debris in the canonical creation order both peers derive.
//
// A note on why the ordinal is safe to lean on: it is only meaningful because m11.4a's A12 fix
// makes the two rosters agree index for index. If a client ever merged two of the authority's ticks
// into one update(), its roster would be built differently and this ordinal would name a different
// chunk — not a slightly-wrong position, an entirely wrong one. The composition hash exists to
// catch that rather than to trust it.
struct DebrisOrigin {
    std::uint32_t source_net_index = 0;
    std::uint32_t source_net_generation = 0;
    std::uint32_t ordinal = 0;
};

// "This mirror is not bound to a local chunk yet" — the DebrisRef null.
inline constexpr std::uint32_t kUnboundDebris = 0xFFFFFFFFu;

// The client's private answer to a DebrisOrigin: which row of ITS debris roster the mirror resolved
// to. Unreflected, for exactly the reason `DestructibleInstanceRef` is — a local table position
// that would name a different chunk on the peer that received it.
struct DebrisRef {
    std::uint32_t debris = kUnboundDebris;
};

// Register the networked-destruction components with a world. Must run on BOTH peers before either
// replicator is constructed: the wire schema is derived from the registry at construction, and the
// schema-hash handshake compares the whole registered set.
inline void register_destruction_net_components(ecs::World& world) {
    (void)world.register_component<DebrisOrigin>();
    (void)world.register_component<DebrisRef>();
}

} // namespace rime::destruction_net

// Reflection (outside the namespace — the macros open rime::core themselves). Only the shared
// identity is reflected; the local roster link is not (see above).
RIME_REFLECT_BEGIN(rime::destruction_net::DebrisOrigin)
RIME_REFLECT_FIELD(source_net_index)
RIME_REFLECT_FIELD(source_net_generation)
RIME_REFLECT_FIELD(ordinal)
RIME_REFLECT_END()
