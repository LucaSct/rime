// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rime/core/byte_cursor.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/net_id.hpp"
#include "rime/replication/snapshot.hpp"
#include "rime/replication/wire_schema.hpp"

// ClientReplicator (m11.3) — the mirror side: creates and destroys local entities to match the
// server's spawn/despawn announcements, writes replicated component state as deltas arrive, and
// reports back the newest tick it holds COMPLETELY.
//
// The client is purely a follower. It never allocates a NetId, never decides an entity exists, and
// never sends state upstream — ADR-0033 §1's server authority, expressed as an API shape rather
// than a rule someone has to remember: there is simply no method here that would let it.
//
// ORDER IS NOT GUARANTEED ACROSS CHANNELS, and this class is where that is absorbed. Spawns arrive
// reliably-ordered; deltas arrive unreliably-sequenced; ADR-0033 §3 gives the two no ordering
// relative to one another *by design*, because the whole point of the split is that state never
// waits behind a resend. So a delta can name a NetId whose Spawn is still in flight.
//
// That record is HELD, not dropped (ADR-0033 A14), and replayed the moment the Spawn binds its id.
// The two weaker answers were both tried first and are worth recording, because the second looked
// right for a while:
//
//   Discard it and wait for the next delta. Correct only for an entity that keeps changing. A wall
//   that is written once and then stands still is never re-offered, so its mirror stays empty
//   forever — the original m11.3 bug.
//
//   Discard it, and refuse to acknowledge the tick so the server re-offers. Correct, and it is what
//   A13 shipped. But it makes acknowledgement hostage to spawn traffic: while entities are
//   arriving, nearly every packet contains an unresolved record, the baseline sits still, and the
//   server re-sends the whole delta every tick. A scene that streams in continuously — which is the
//   scene this engine is for — would pay that permanently.
//
// Holding the bytes costs a bounded buffer and makes the question go away: nothing is lost, so the
// tick is honestly complete and can be acknowledged. The only case that still suppresses an
// acknowledgement is buffer exhaustion, which an honest peer never reaches.
namespace rime::replication {

class ClientReplicator {
public:
    // `world` must outlive the replicator. As on the server, every replicable component must be
    // registered before construction — the wire schema is derived from the registry here, and the
    // schema-hash handshake has already proved the two registries match.
    explicit ClientReplicator(ecs::World& world);

    // Apply everything the server has sent: spawns, despawns, and state. Call from PreSim, after
    // NetDriver::update, so the local tick runs against corrected state rather than a tick behind.
    //
    // This convenience form DRAINS the sessions, which means it takes sole ownership of the mail.
    // Use it only when replication is the single tenant of the session. The moment another module
    // also has messages to read — m11.4's damage-op stream is the first — the app must drain once
    // itself and hand the same span to each subsystem via apply_messages below, because
    // drain_received moves messages out and the second caller would find an empty inbox.
    void apply_inbound(net::NetDriver& driver);

    // Apply the replication messages out of an already-drained batch, leaving everything else
    // untouched for whoever else is reading the same span. Tags outside replication's block of the
    // shared registry (snapshot.hpp) are counted as `foreign` and otherwise ignored — deliberately
    // NOT as malformed, since another module's perfectly well-formed message is not an error.
    void apply_messages(std::span<const net::Received> messages);

    // Report the newest COMPLETE tick we hold. Call from Publish. Cheap enough to send every tick
    // (9 bytes), and sending it unconditionally is what keeps the server's baseline from drifting
    // stale during a quiet stretch when nothing else is flowing.
    void send_ack(net::NetDriver& driver, std::uint64_t now_ms);

    [[nodiscard]] const NetIdMap& map() const noexcept { return map_; }

    [[nodiscard]] ecs::Version watermark() const noexcept { return acks_.watermark(); }

    // Counters — the proof asserts on these, because a loss test in which nothing was ever dropped
    // proves nothing (the m11.1 harness discipline).
    [[nodiscard]] std::uint64_t spawns_applied() const noexcept { return spawns_applied_; }

    [[nodiscard]] std::uint64_t despawns_applied() const noexcept { return despawns_applied_; }

    [[nodiscard]] std::uint64_t deltas_applied() const noexcept { return deltas_applied_; }

    // Delta records naming a NetId this client cannot resolve — the cross-channel race above.
    // Expected to be non-zero in any test with real loss; a zero here in such a test means the
    // interesting path was never exercised.
    [[nodiscard]] std::uint64_t records_dropped_unmapped() const noexcept {
        return records_dropped_unmapped_;
    }

    [[nodiscard]] std::uint64_t malformed_messages() const noexcept { return malformed_; }

    // Records held because their Spawn had not landed yet, and later applied when it did. This is
    // the healthy path, not an error: it is what lets a tick be acknowledged even though part of it
    // could not be applied on arrival.
    [[nodiscard]] std::uint64_t records_deferred() const noexcept { return records_deferred_; }

    [[nodiscard]] std::uint64_t records_replayed() const noexcept { return records_replayed_; }

    // Deferred records evicted because the buffer was full — the only case that still suppresses an
    // acknowledgement. Should be zero against an honest peer.
    [[nodiscard]] std::uint64_t records_evicted() const noexcept { return records_evicted_; }

    // Messages belonging to another module's tag block that we passed over. Non-zero the moment a
    // session carries more than replication, and the counter a proof asserts on to show the two
    // streams really are sharing one session rather than one having quietly starved the other.
    [[nodiscard]] std::uint64_t foreign_messages() const noexcept { return foreign_; }

private:
    void on_spawn(core::ByteReader& reader);

    // Apply every record held for `id`, oldest first, and drop them. Called the moment a Spawn
    // binds the id — arrival order is preserved, so the newest state still wins.
    void replay_deferred(NetId id, ecs::Entity local);

    // One component write that arrived before the entity it belongs to. The bytes are COPIED: the
    // reader's span points into a datagram buffer that is reused the moment this call returns.
    struct DeferredRecord {
        NetId id{};
        ecs::ComponentId component{};
        std::vector<std::byte> bytes;
    };

    void on_despawn(core::ByteReader& reader);
    void on_delta(core::ByteReader& reader);

    ecs::World* world_;
    WireSchema schema_;
    NetIdMap map_;
    ecs::ComponentId replicated_id_{};
    AckTracker acks_;

    std::vector<net::Received> inbox_;
    std::vector<std::byte> scratch_;

    std::uint64_t spawns_applied_ = 0;
    std::uint64_t despawns_applied_ = 0;
    std::uint64_t deltas_applied_ = 0;
    std::uint64_t records_dropped_unmapped_ = 0;
    std::uint64_t malformed_ = 0;
    std::uint64_t foreign_ = 0;
    std::uint64_t records_deferred_ = 0;
    std::uint64_t records_replayed_ = 0;
    std::uint64_t records_evicted_ = 0;

    // Records waiting for their Spawn, in arrival order. Bounded (see kMaxDeferredRecords): an
    // unbounded buffer keyed by ids a peer chooses is a peer-controlled allocation, which is the
    // shape of a denial-of-service rather than of a resilience feature.
    std::vector<DeferredRecord> deferred_;
};

} // namespace rime::replication
