// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "rime/core/containers/handle.hpp"
#include "rime/ecs/entity.hpp"

// Network identity (m11.3, ADR-0033 §4) — the name an entity answers to *on the wire*, and the two
// maps that translate between it and each side's purely local `ecs::Entity`.
//
// WHY A SEPARATE ID AT ALL. The server and each client run independent `ecs::World`s that allocate
// entity slots on their own schedules; the same wall in two processes is almost never the same
// `Entity`. So the wire cannot name entities — it names `NetId`s, which the server assigns and
// every client binds to whatever local entity it happened to create as that thing's mirror.
//
// WHY A GENERATIONAL HANDLE, AND NOT A MONOTONIC COUNTER. A never-recycled counter is simpler and
// needs no generation field, but its id space — and any table indexed by it — grows with the
// *cumulative* number of spawns over a match rather than the *peak concurrent* count. Destruction
// is exactly the workload that makes those diverge: debris spawns and dies continuously, so a
// cumulative bound is unbounded in practice. Recycling indices bounds the table by peak
// concurrency, and `core::Handle`'s generation stamp is what makes recycling safe — the same
// machinery, and the same reasoning, that `ecs::Entity` itself is built on.
//
// The generation is not decoration; it closes a real hole. See NetIdMap::resolve.
namespace rime::replication {

// Phantom tag so a NetId is its own type in the handle family — it can never be crossed with an
// `ecs::Entity`, which it is structurally identical to (both are {index, generation}).
struct NetIdTag {};

// The wire name of a replicated entity: an 8-byte generational handle, server-assigned.
using NetId = core::Handle<NetIdTag>;

// The null NetId: structurally invalid, never names a live replicated entity.
inline constexpr NetId kNullNetId{};

// Server-side allocation of NetIds. The client never constructs one — it only ever receives NetIds
// inside spawn messages and binds them (ADR-0033 §4: "server creates → assigns → clients bind on
// spawn"), which is what makes network identity unforgeable by a client.
//
// The free list is LIFO and reuse is immediate, mirroring `ecs::EntityDirectory` exactly. There is
// deliberately no cooldown before an index may be reused: a cooldown is a mitigation for a system
// that cannot *detect* stale references, and this one can — see NetIdMap::resolve.
class NetIdAllocator {
public:
    // Allocate a fresh NetId, reusing a freed index when one is available. The returned handle's
    // generation is that slot's current stamp, so it differs from every id previously issued for
    // the same index.
    [[nodiscard]] NetId allocate();

    // Release `id`, bumping its slot's generation so every copy of the old handle goes stale, and
    // returning the index to the free list. A no-op for an id that is already dead or was never
    // issued — releasing twice must not corrupt the generation sequence.
    void free(NetId id) noexcept;

    // True iff `id` names a currently-allocated slot at the matching generation.
    [[nodiscard]] bool is_live(NetId id) const noexcept;

    [[nodiscard]] std::size_t live_count() const noexcept { return live_count_; }

    // How many slot indices have ever been handed out — the bound for an index-addressed walk. Not
    // the live count: a freed index keeps its slot so its generation survives to reject stale
    // handles.
    [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }

    // The live NetId occupying `index`, or kNullNetId if that slot is free or out of range. Lets a
    // caller diff "what I have announced" against "what exists" by index, which is what makes the
    // server's spawn/despawn announcements self-healing rather than an event queue that has to be
    // repaired when a send is refused.
    [[nodiscard]] NetId live_id_at(std::uint32_t index) const noexcept;

private:
    struct Slot {
        std::uint32_t generation = 0;
        bool alive = false;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t live_count_ = 0;
};

// The NetId ↔ Entity translation table. One per replication peer relationship: the client keeps one
// (for the server's ids), and the server keeps one shared table of what it has published.
//
// Two access patterns, so two structures. `by_index_` is a dense vector addressed by NetId::index —
// the hot path, hit once per applied snapshot record. `by_entity_` is a hash map for the cold
// reverse lookup (a gameplay despawn asks "what NetId was this?"), which happens per structural
// change rather than per tick.
class NetIdMap {
public:
    // Associate `net_id` with the local entity mirroring it. Rebinding an index that already holds
    // a live binding replaces it — the caller is expected to unbind first, but a replace is the
    // safe interpretation rather than an assert, because the wire can legitimately re-announce a
    // spawn (a resend the receiver already applied).
    void bind(NetId net_id, ecs::Entity local);

    // Drop the binding for `net_id`. A no-op if nothing is bound, or if a *newer* incarnation
    // occupies that index — an unbind for a stale generation must not evict its successor.
    void unbind(NetId net_id) noexcept;

    // Resolve `net_id` to the local entity mirroring it, or kNullEntity if it names nothing here.
    //
    // THE GENERATION CHECK IS LOAD-BEARING, and it closes a hole that is invisible until you hold
    // the two channel contracts side by side. Spawn/despawn ride the reliable-ordered channel;
    // snapshots ride the unreliable-sequenced one; ADR-0033 §3 gives the two *no ordering relative
    // to each other*, and that is deliberate — the whole point of the split is that unreliable
    // traffic never waits on reliable resends.
    //
    // So: the server despawns NetId{7,g1}, immediately recycles index 7 as NetId{7,g2}, and sends
    // Spawn{7,g2} reliably — which under loss may take several round trips. Nothing stops an
    // unreliable snapshot carrying {7,g2}'s data from arriving *first*. A map keyed on index alone
    // would cheerfully write the new entity's state onto the stale local mirror of the old one:
    // a silent, wire-shaped corruption. Comparing the generation turns that into a clean miss —
    // the record is dropped, the entity stays one tick stale, and the reliable Spawn puts the
    // binding right when it lands (exactly once, in order, guaranteed).
    [[nodiscard]] ecs::Entity resolve(NetId net_id) const noexcept;

    [[nodiscard]] std::optional<NetId> net_id_of(ecs::Entity local) const;

    [[nodiscard]] std::size_t size() const noexcept { return by_entity_.size(); }

    // Every live binding, for the convergence hash and for teardown. Order is by NetId index, which
    // is stable across processes — unlike local Entity order, which is not.
    void for_each(const std::function<void(NetId, ecs::Entity)>& fn) const;

    void clear() noexcept;

private:
    struct Slot {
        std::uint32_t generation = 0;
        ecs::Entity local = ecs::kNullEntity;
        bool bound = false;
    };

    // Hash for ecs::Entity — it is two u32s and has no std::hash specialization (and deliberately
    // should not grow one for a single consumer's sake; the same call ADR-0033 A7 made about
    // platform::Endpoint).
    struct EntityHash {
        [[nodiscard]] std::size_t operator()(ecs::Entity e) const noexcept {
            return (static_cast<std::size_t>(e.index) << 32) ^ e.generation;
        }
    };

    std::vector<Slot> by_index_;
    std::unordered_map<ecs::Entity, NetId, EntityHash> by_entity_;
};

} // namespace rime::replication
