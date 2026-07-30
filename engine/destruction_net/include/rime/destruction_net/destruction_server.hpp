// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/destruction/world.hpp"
#include "rime/destruction_net/wire.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/net_id.hpp"

// DestructionServer (m11.4a) — the authority half of networked destruction: publish the damage-op
// list this tick committed, addressed by NetId, on the reliable-ordered channel.
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

    // Counters, so a proof can assert the mechanism fired rather than that nothing broke.
    [[nodiscard]] std::uint64_t op_packets_sent() const noexcept { return packets_sent_; }

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

    std::uint64_t packets_sent_ = 0;
    std::uint64_t ops_sent_ = 0;
    std::uint64_t multipart_ticks_ = 0;
    std::uint64_t unaddressable_ = 0;
};

} // namespace rime::destruction_net
