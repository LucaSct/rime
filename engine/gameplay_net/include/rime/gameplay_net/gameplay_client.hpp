// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rime/ecs/entity.hpp"
#include "rime/gameplay_net/wire.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/net_id.hpp"

namespace rime::ecs {
class World;
}

// GameplayClient v1 (m12.3) — the follower half of the networked player.
//
// IT IS DELIBERATELY ALMOST NOTHING, and the emptiness is the brick's point. m12.3 is
// server-authoritative with NO PREDICTION: the client samples input, sends it (that is
// `replication::ClientInputSender`, built at m11.6c), and then simply *watches* its own avatar
// arrive through the ordinary snapshot path like any other entity. So the only thing this class
// does is answer two questions the snapshot cannot:
//
//   "which of these entities is me?"   — from the AssignPlayer message (wire.hpp).
//   "how stale is what I am looking at?" — from the replicated `LastProcessedInput` on that entity.
//
// The second is the honest v1 reading of the pairing m12.4 will reconcile against, and it is what
// the proof measures: the gap between the sequence a client has SENT and the sequence the state it
// HOLDS was computed from is own-input latency, in commands, with no clock synchronisation
// anywhere (ADR-0033 A11 rules a clock offset out, and M12 does not need one).
//
// `Predictor` — the ring of {sequence, state}, the tolerance gate, the rewind and the replay —
// lands here at m12.4. It is named and absent rather than sketched: A20's whole argument is that a
// guessed interface in a header is inherited as a constraint, and this file would rather be
// obviously incomplete than plausibly wrong.
namespace rime::gameplay_net {

class GameplayClient {
public:
    // Read this module's messages out of an already-drained batch, leaving the rest of the span for
    // other readers. Returns how many were consumed.
    //
    // The shared-inbox contract, same as everywhere else: a client drains its session ONCE and
    // hands the same span to each reader (ClientReplicator, DestructionClient, ClientInputSender,
    // this). `drain_received` MOVES messages out, so a second drain finds an empty inbox — the
    // failure mode being that whichever subsystem drained second silently never receives its mail.
    std::size_t apply_messages(std::span<const net::Received> messages);

    // Convenience for a client whose session carries only this module's traffic. A real client has
    // several readers and must drain once itself, then share the span with `apply_messages`.
    std::size_t apply_inbound(net::NetDriver& driver);

    // The wire name of this client's own avatar, or kNullNetId if the server has not said yet.
    [[nodiscard]] replication::NetId local_player_id() const noexcept { return local_player_; }

    // The local entity mirroring it, or kNullEntity if the assignment has not arrived or its Spawn
    // has not landed yet.
    //
    // RESOLVED ON EVERY ASK, never cached — see wire.hpp. Reliable-ordered delivery does not order
    // this message against the unreliable Delta stream, so an assignment can legitimately arrive
    // before the entity exists; caching the null answer would make "I do not know who I am" a
    // permanent state instead of a one-tick one. The map lookup is a vector index.
    [[nodiscard]] ecs::Entity local_player(const replication::NetIdMap& map) const noexcept;

    // The newest input sequence the server had CONSUMED when it computed the state this client
    // currently holds for its own avatar. Zero if the assignment has not arrived, the entity is not
    // bound yet, or the server has not yet acted on any of this client's commands.
    //
    // Read from the replicated component rather than from `ClientInputSender::acked_through`, and
    // the difference matters: the ack is a fresher, unreliable, session-level frontier that says
    // nothing about which state you are holding, while this number is welded to the state it
    // describes because it arrived in the same record. Reconciling against the ack is the mistake
    // components.hpp exists to argue against.
    [[nodiscard]] std::uint32_t last_processed_input(const ecs::World& world,
                                                     const replication::NetIdMap& map) const;

    // Forget the assignment — call on disconnect, so a reconnecting client does not drive a stale
    // NetId for a tick. (The generation check inside `NetIdMap::resolve` already makes that a miss
    // rather than a mis-resolution; this makes it a miss immediately rather than eventually.)
    void reset() noexcept;

    // Counters. Assignments are re-sent on reconnect and the receiver overwrites, so this is a
    // count of messages rather than of distinct avatars.
    [[nodiscard]] std::uint64_t assignments_received() const noexcept {
        return assignments_received_;
    }

    [[nodiscard]] std::uint64_t malformed_messages() const noexcept { return malformed_; }

private:
    replication::NetId local_player_ = replication::kNullNetId;
    std::vector<net::Received> inbox_;
    std::uint64_t assignments_received_ = 0;
    std::uint64_t malformed_ = 0;
};

} // namespace rime::gameplay_net
