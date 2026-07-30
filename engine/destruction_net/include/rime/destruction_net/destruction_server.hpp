// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "rime/destruction/world.hpp"
#include "rime/destruction_net/components.hpp"
#include "rime/destruction_net/wire.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/net_id.hpp"

namespace rime::physics {
class PhysicsWorld;
}

// DestructionServer (m11.4a/b) — the authority half of networked destruction: publish the damage-op
// list this tick committed, addressed by NetId, on the reliable-ordered channel (m11.4a), and keep
// the debris↔entity bridge that lets m11.3 replicate the rubble's transforms (m11.4b).
//
// It is deliberately small and stateless-per-tick. Everything that makes destruction replicable was
// already built by the pieces underneath: `DestructionWorld::committed_ops()` produces the
// canonical list (ADR-0033 A1), the bind table translates instance → entity, and m11.3's NetIdMap
// translates entity → the name both peers share. This class is the join, plus the packing.
//
// It does NOT own replication of the destructible ENTITIES themselves — the server calls
// `ServerReplicator::replicate` on them like any other entity, and their `Destructible{asset}`
// component reaches clients through the ordinary snapshot path. That is the whole reason the bind
// path exists: structure travels as ECS state, and only the damage transitions need a channel of
// their own.
namespace rime::destruction_net {

class DestructionServer {
public:
    // Publish the ops `destruction.update()` just committed to every connected client. Call from
    // Publish, AFTER the destruction update — `committed_ops()` describes the tick that just ran,
    // and reading it before the update would ship the previous tick's list one tick late, forever.
    //
    // `tick` is the batch's identity and ordering key, not a schedule (see wire.hpp / ADR-0033
    // A11). Pass the world version or tick counter the rest of the frame uses; it only has to be
    // monotonic.
    //
    // A tick that committed no ops sends nothing at all. Destruction is bursty — a quiet minute
    // between two collapses is the common case, and a per-tick keepalive on the RELIABLE channel
    // would be the worst possible thing to spend the 32-packet in-flight window on.
    void publish(net::NetDriver& driver,
                 const replication::NetIdMap& map,
                 const ecs::World& world,
                 const destruction::DestructionWorld& destruction,
                 std::uint64_t tick,
                 std::uint64_t now_ms);

    // Keep the debris↔entity bridge in step with the roster (m11.4b). Call from PostSim, after the
    // destruction update that may have created or reclaimed chunks, and BEFORE the replicators
    // publish — the entities it spawns must carry this tick's transform, not last tick's.
    //
    // It does three things, all idempotent: spawn a replicated entity for each new chunk (carrying
    // `DebrisOrigin` so the receiver can tell which of ITS chunks the transform is about), refresh
    // every live chunk's `LocalTransform` from its physics body, and retract the entities of chunks
    // the M8.5 lifecycle has reclaimed.
    //
    // `replicate` and `despawn` are passed as callbacks rather than taking a `ServerReplicator&`
    // because that is the entire dependency this module would otherwise have on the replicator's
    // mutable API — and keeping it a function boundary means a game with its own relevancy policy
    // (m11.5) can decide per chunk whether to opt it in at all.
    void sync_debris(ecs::World& world,
                     const destruction::DestructionWorld& destruction,
                     const physics::PhysicsWorld& physics,
                     const replication::NetIdMap& map,
                     const std::function<void(ecs::Entity)>& replicate,
                     const std::function<void(ecs::Entity)>& despawn);

    // Counters, so a proof can assert the mechanism fired rather than that nothing broke.
    [[nodiscard]] std::uint64_t op_packets_sent() const noexcept { return packets_sent_; }

    [[nodiscard]] std::uint64_t composition_checks_sent() const noexcept {
        return composition_checks_sent_;
    }

    [[nodiscard]] std::uint64_t debris_entities_spawned() const noexcept { return debris_spawned_; }

    [[nodiscard]] std::uint64_t debris_entities_retracted() const noexcept {
        return debris_retracted_;
    }

    // Chunks that could not be given a `DebrisOrigin` because their SOURCE destructible carries no
    // NetId. Their transforms cannot be addressed, so they are left unreplicated rather than
    // replicated under a name the receiver cannot resolve.
    [[nodiscard]] std::uint64_t debris_unaddressable() const noexcept {
        return debris_unaddressable_;
    }

    [[nodiscard]] std::uint64_t ops_sent() const noexcept { return ops_sent_; }

    [[nodiscard]] std::uint64_t multipart_ticks() const noexcept { return multipart_ticks_; }

    // Ops whose instance had no bound entity, or whose entity carried no NetId — they could not be
    // named on the wire, so they were dropped. Non-zero means a destructible is taking damage that
    // clients will never hear about, which is a setup bug (an unreplicated destructible) and worth
    // surfacing loudly rather than discovering as a wall that breaks on one machine only.
    [[nodiscard]] std::uint64_t ops_unaddressable() const noexcept { return unaddressable_; }

private:
    // Reused across ticks so the steady state allocates nothing.
    std::vector<ecs::Entity> instance_to_entity_;
    std::vector<std::byte> scratch_;

    // Roster index → the entity standing for that chunk, or a null entity for a row that has none
    // (never had one, or has been retracted). Indexed directly: the roster is append-only within a
    // run and its rows never shift, which is the property the M8.5 lifecycle preserves by keeping a
    // frozen chunk's record rather than erasing it.
    std::vector<ecs::Entity> debris_to_entity_;

    std::uint64_t packets_sent_ = 0;
    std::uint64_t ops_sent_ = 0;
    std::uint64_t multipart_ticks_ = 0;
    std::uint64_t unaddressable_ = 0;
    std::uint64_t debris_spawned_ = 0;
    std::uint64_t debris_retracted_ = 0;
    std::uint64_t debris_unaddressable_ = 0;
    std::uint64_t composition_checks_sent_ = 0;
};

} // namespace rime::destruction_net
