// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/component.hpp"

// WireSchema (m11.3) — the translation between a component type's LOCAL identity and its identity
// ON THE WIRE, plus the cached facts a snapshot reader needs about it.
//
// WHY ecs::ComponentId CANNOT GO ON THE WIRE. ComponentIds are handed out in registration order
// (component.hpp: "a small dense integer assigned in registration order"), and m11.2 deliberately
// made registration order a NON-CONTRACT: `ecs::component_schema_hash` folds a *sorted* list of
// type hashes precisely so that two peers registering the same components in different orders still
// agree. Two builds can therefore share an identical component set while disagreeing about every
// single ComponentId. Putting one on the wire would be reading the right bytes into the wrong type.
//
// WHAT WE SEND INSTEAD, FOR FREE. Sort the reflected components by their (name-folded, ADR-0033 A2)
// `type_hash` and use each one's RANK in that ordering. Both peers compute it independently from
// their own registry, and they are guaranteed to agree — because the handshake already proved their
// component sets are identical, and sorting by type_hash is deterministic. Nothing about the table
// is ever transmitted: it is derived, not negotiated. This is `component_schema_hash`'s own
// order-independence trick applied one level down.
//
// A nice consequence worth noticing: `NetId` is {index, generation}, structurally identical to
// `ecs::Entity`. Before A2 folded the type name into the fingerprint those two would have collided
// on one hash and this ranking would have been ambiguous. This brick is a direct beneficiary of
// that fix, not a hypothetical one.
namespace rime::replication {

// A component type's index in the type_hash-sorted replicated set. Dense and small, so a snapshot
// record spends two bytes naming a type rather than the eight a raw type_hash would cost.
enum class WireComponentId : std::uint16_t {};

inline constexpr WireComponentId kInvalidWireComponentId{0xFFFFu};

class WireSchema {
public:
    // Derive the table from a registry. Deterministic: the same registry contents always produce
    // the same table, on any platform, in any registration order.
    [[nodiscard]] static WireSchema build(const ecs::ComponentRegistry& registry);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // The wire id for a local component id, or kInvalidWireComponentId if that component is not
    // replicable (unreflected, or excluded — see `excluded_names`).
    [[nodiscard]] WireComponentId wire_id_of(ecs::ComponentId local) const noexcept;

    // The inverse, for the reader. Returns false if `wire` is out of range — which, given the
    // schema-hash handshake, means the peer is lying rather than merely out of date, so callers
    // drop the packet.
    [[nodiscard]] bool lookup(WireComponentId wire,
                              ecs::ComponentId& local_out,
                              const core::TypeInfo*& type_out,
                              std::size_t& packed_size_out) const noexcept;

    // Reflected component types that were deliberately LEFT OUT of the replicated set, by name.
    //
    // Today that is exactly one category: any type containing an `ecs::Entity` field, at any depth.
    // Such a field holds a handle into the *sender's* entity directory, which names nothing (or,
    // worse, names something unrelated) in the receiver's. `ecs::Parent` is the live example —
    // replicating a hierarchy needs the reference translated through the NetIdMap on the way out
    // and back on the way in, which is real work and a named follow-up, not something to fake by
    // shipping the raw handle and hoping.
    //
    // Exposed rather than merely skipped so a server can say so out loud at startup: silently
    // dropping a component the game thought was replicating is exactly the kind of bug that costs
    // an afternoon.
    [[nodiscard]] const std::vector<std::string>& excluded_names() const noexcept {
        return excluded_names_;
    }

    // True iff `type` (recursively) contains no ecs::Entity-shaped field. Exposed for tests and for
    // a game that wants to check a component before relying on it replicating.
    [[nodiscard]] static bool is_replicable(const core::TypeInfo& type) noexcept;

private:
    struct Entry {
        ecs::ComponentId local{};
        const core::TypeInfo* type = nullptr;
        std::size_t packed_size = 0;
    };

    // Index == WireComponentId value, ordered by ascending type_hash.
    std::vector<Entry> entries_;
    // Local ComponentId → wire index, or kInvalidWireComponentId. Dense: ComponentIds are already
    // a compact 0..N range, so a vector beats a map and needs no hashing on the hot path.
    std::vector<WireComponentId> by_local_;
    std::vector<std::string> excluded_names_;
};

} // namespace rime::replication
