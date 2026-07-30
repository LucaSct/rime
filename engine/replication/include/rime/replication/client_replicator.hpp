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
// waits behind a resend. So a delta can name a NetId whose Spawn is still in flight. That is not an
// error and is not logged as one — `NetIdMap::resolve` returns null, the record is dropped, and the
// entity simply catches up on the next delta after its Spawn lands.
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

    // Messages belonging to another module's tag block that we passed over. Non-zero the moment a
    // session carries more than replication, and the counter a proof asserts on to show the two
    // streams really are sharing one session rather than one having quietly starved the other.
    [[nodiscard]] std::uint64_t foreign_messages() const noexcept { return foreign_; }

private:
    void on_spawn(core::ByteReader& reader);
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
};

} // namespace rime::replication
