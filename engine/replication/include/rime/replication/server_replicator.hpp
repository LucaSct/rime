// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/net_id.hpp"
#include "rime/replication/snapshot.hpp"
#include "rime/replication/wire_schema.hpp"

// ServerReplicator (m11.3) — the authority side: assigns NetIds, tells each client which entities
// exist, and publishes the component state that changed since that client last confirmed it.
//
// THE DELTA MECHANISM, AND WHY IT COSTS ONE INTEGER PER CLIENT. The obvious way to answer "what has
// changed since the client's baseline" is to keep a ring of past snapshots per client and diff
// against the acked one — O(clients × history × entities), which is where classic engines spend
// real memory. Rime does not need it: `ecs::Chunk` already stamps every component COLUMN with the
// world version it was last written at (ADR-0018 §4, built for the editor and for transform
// dirtying). That is exactly "what changed since an arbitrary point in the past", at O(1) memory,
// for any point in the past. So the per-client state here is a single `ecs::Version` — the baseline
// they have acknowledged — and the delta is recomputed by comparing column stamps against it.
//
// The cost that IS real, named honestly: the comparison pass runs once per client per tick and does
// not amortize across clients whose baselines have diverged (they will, under differing RTT and
// loss). It is an integer compare per (chunk, column), so it is cheap per unit — but at the ADR's
// 64-player target this is the first place a profiler should be pointed, ahead of bandwidth.
// m11.5's relevancy work shrinks this same loop (a client walks only chunks relevant to it), so the
// two bricks compose rather than duplicate.
//
// KNOWN COARSENESS. The change-detection grain is the chunk column, not the row. An entity that did
// not move but shares a chunk with entities that did will have its component re-sent, because the
// column stamp is chunk-wide. Bounded by chunk occupancy, and the mitigation costs no code — keep
// continuously-moving entities out of archetypes dominated by static ones. Worth measuring before
// m11.4's debris makes it urgent.
namespace rime::replication {

// Score one client's interest in each candidate entity — the m11.5 relevancy seam.
//
// CALLED ONCE PER CLIENT PER TICK, over the whole candidate span, filling `priorities` in place.
// Deliberately not once per entity: at the ADR's 64-player target with a few thousand replicated
// entities, a per-entity indirect call is millions of them a tick, and the policy would become the
// profile. One call per client leaves the policy free to be vectorized, spatially indexed, or
// answered from a precomputed grid — none of which it could do if it were only ever shown one
// entity at a time.
//
//   priorities[i] >  0   relevant; larger is sent sooner when the budget binds
//   priorities[i] <= 0   not relevant to this client this tick; not sent at all
//
// Leave it unset and every entity is relevant at equal priority, which is exactly the m11.3/11.4
// behaviour. `distance_relevancy` in relevancy.hpp is a ready-made policy; a game that wants
// team-based, portal-based, or PVS relevancy writes its own and never touches this module.
using RelevancyFn =
    std::function<void(net::SessionId, std::span<const ecs::Entity>, std::span<float> priorities)>;

// Per-client outbound limits. Zero means unlimited, which is the pre-m11.5 behaviour.
struct Budget {
    // Hard ceiling on delta payload bytes to one client in one tick. Enforced AFTER priority
    // ordering, so what survives a tight budget is what the policy said mattered most — the
    // difference between a budget that degrades gracefully and one that truncates arbitrarily.
    std::size_t max_bytes_per_tick = 0;
};

class ServerReplicator {
public:
    // `world` must outlive the replicator. The wire schema is derived from the world's component
    // registry at construction, so every component the game intends to replicate must be registered
    // BEFORE this is built — the same ordering the schema-hash handshake already requires.
    explicit ServerReplicator(ecs::World& world);

    // Opt `entity` into replication: assign it a NetId, bind it, and tag it `Replicated`. Returns
    // the assigned id (kNullNetId if the entity is not alive).
    NetId replicate(ecs::Entity entity);

    // Despawn a replicated entity — USE THIS, never `world.despawn()` directly, for anything
    // carrying a NetId. It retracts the id before destroying the entity, so clients are told to
    // drop their mirrors; a bare `world.despawn` would leave a phantom on every client forever,
    // with nothing that ever repairs it. (A checked-build backstop for the mistake is a named
    // follow-up; today this is a documented discipline, which is worth stating plainly rather than
    // pretending the type system enforces it.)
    void despawn(ecs::Entity entity);

    // Drain session lifecycle: a newly Connected peer starts from nothing, a Disconnected one has
    // its per-client state reclaimed. Call from PreSim with the events NetDriver::update produced.
    void on_session_events(const std::vector<net::SessionEvent>& events);

    // Drain client→server replication traffic (today: BaselineAck). Call from PreSim, after
    // NetDriver::update. Returns how many acks were consumed.
    //
    // Like the client's, this form takes sole ownership of the mail: drain_received moves messages
    // out. Once anything else on this peer also reads client→server traffic, drain once in the app
    // and fan the span out through apply_messages instead.
    std::size_t apply_inbound(net::NetDriver& driver);

    // Apply the replication messages in an already-drained batch from session `id`, leaving the
    // rest of the span for other readers. Returns how many acks were consumed.
    std::size_t apply_messages(net::SessionId id, std::span<const net::Received> messages);

    // Install the relevancy policy (see RelevancyFn). Takes effect from the next publish.
    void set_relevancy(RelevancyFn fn);

    // Install the per-client outbound budget. Takes effect from the next publish.
    void set_budget(const Budget& budget);

    // Announce structure and publish state to every connected client. Call from Publish — after
    // everything the tick will mutate has mutated, so the state described is the tick's final
    // state rather than a version of it that self-corrects next tick.
    void publish(net::NetDriver& driver, std::uint64_t now_ms);

    [[nodiscard]] const NetIdMap& map() const noexcept { return map_; }

    [[nodiscard]] const WireSchema& schema() const noexcept { return schema_; }

    // Per-client baseline, for tests and diagnostics: the newest tick this client has confirmed a
    // COMPLETE snapshot of. Returns 0 for an unknown session or one that has never acked.
    [[nodiscard]] ecs::Version acked_baseline(net::SessionId id) const noexcept;

    // Per-client delivery watermark, for tests and diagnostics: the newest tick this client was
    // sent everything it was owed. Lags `acked_baseline` whenever relevancy or the byte budget is
    // withholding, and that gap is the honest measure of how far behind a client is being kept.
    [[nodiscard]] ecs::Version complete_through(net::SessionId id) const noexcept;

    // Counters, so a proof can assert the mechanism actually fired rather than that nothing broke.
    [[nodiscard]] std::uint64_t delta_packets_sent() const noexcept { return delta_packets_sent_; }

    [[nodiscard]] std::uint64_t multipart_ticks() const noexcept { return multipart_ticks_; }

    [[nodiscard]] std::uint64_t full_reseeds() const noexcept { return full_reseeds_; }

    // Entity records deferred to a later tick because they did not fit — counting BOTH budgets, the
    // per-tick byte ceiling and the per-tick packet-count ceiling. "Dropped" names what happened to
    // the record, not to the state: the deferred entity stays in the candidate set and the rotation
    // cursor resumes there, so this is a latency measure, not a loss one. It reading zero under a
    // tight budget means the budget is not binding, which is the thing a proof most wants to know.
    //
    // The byte-budget half of this went uncounted until m11.5's close-out, which is how it hid a
    // watermark bug for a whole brick: the trim happened, nothing recorded it, and the tick then
    // looked complete to everything downstream.
    [[nodiscard]] std::uint64_t entities_dropped_over_budget() const noexcept {
        return entities_over_budget_;
    }

    // Entities skipped because the relevancy policy scored them non-positive for that client. Not a
    // fault — it is the mechanism working — but a proof asserts on it, because a relevancy test in
    // which nothing was ever culled proves only that the unfiltered path still works.
    [[nodiscard]] std::uint64_t entities_culled_irrelevant() const noexcept {
        return entities_culled_;
    }

    // Entities sent because they ENTERED a client's relevant set, rather than because they changed.
    // The counter that shows the version-delta and relevancy are composing rather than fighting.
    [[nodiscard]] std::uint64_t entities_sent_on_entry() const noexcept {
        return entities_entered_;
    }

    // Records emitted by the ENTRY PASS — the targeted walk that serializes exactly the entities
    // newly relevant to a client, rather than widening the chunk scan for everyone (see
    // publish_delta).
    //
    // This replaced a counter called `full_walk_ticks`, and the reason is worth keeping. The old
    // design gated the per-chunk "changed since baseline" skip on a single global flag: if anything
    // anywhere entered relevance, the whole replicated world was re-examined for that client. That
    // counter existed to watch for the flag getting stuck on — a defect with no wrong output at
    // all, since the bytes are identical and only the cost moves. It did get stuck, twice.
    //
    // The entry pass removes the flag rather than watching it, so the failure it guarded is now
    // unrepresentable: there is no widening left to be stuck. What is worth counting instead is the
    // real work — how many records the entry path actually produced. In a steady state with a
    // stationary viewpoint this should sit at zero; sustained non-zero means relevancy is churning.
    [[nodiscard]] std::uint64_t entry_pass_records() const noexcept { return entry_pass_records_; }

    // Publishes that ran the delta path at all, so `entry_pass_records` has a denominator. Counted
    // per (client, tick), including ticks that produced no records.
    [[nodiscard]] std::uint64_t delta_ticks() const noexcept { return delta_ticks_; }

private:
    struct ClientState {
        net::SessionId id{};
        bool in_use = false;
        ecs::Version acked_baseline = 0; // 0 = nothing confirmed; every column stamp beats it

        // The newest tick at which this client was sent EVERYTHING it was owed. The effective
        // baseline is min(acked_baseline, complete_through), and that clamp is what makes
        // withholding safe (see publish_delta).
        //
        // Symmetric with the client's AckTracker watermark, and for the same reason: an
        // acknowledgement is a promise about state APPLIED, and a tick the server deliberately sent
        // only part of cannot be allowed to satisfy it — however honestly the client acked what it
        // actually received.
        ecs::Version complete_through = 0;

        // Where in the candidate list to resume packing. Without it, a world permanently over
        // budget sends the same prefix every tick and the tail is never delivered at all — the
        // baseline clamp alone gives correctness of the ack but not LIVENESS of delivery.
        std::size_t cursor = 0;

        // Indexed by NetId::index: was this entity relevant to this client last tick?
        //
        // This is what makes relevancy safe to combine with a version-based delta. An entity that
        // was irrelevant and becomes relevant has, by definition, not changed since the client's
        // baseline — it was simply never sent — so the ordinary "changed since" test excludes it
        // and the client would mirror an entity it has no state for. Transitioning into the
        // relevant set therefore forces a send regardless of version. This is the per-client
        // bookkeeping m11.5 was always going to need, and it is why relevancy could not be a pure
        // filter.
        std::vector<std::uint8_t> was_relevant;
        // Indexed by NetId::index: the generation this client has been told about, or 0 for "never
        // announced". Diffing this against the allocator each tick is what makes spawn/despawn
        // announcements self-healing — a client that missed an announcement is simply re-diffed
        // into the right state rather than needing a repair protocol.
        std::vector<std::uint32_t> announced;
    };

    ClientState* find_client(net::SessionId id) noexcept;
    ClientState& client_for(net::SessionId id);

    void publish_structure(net::Session& session, ClientState& state, std::uint64_t now_ms);
    void publish_delta(net::Session& session, ClientState& state, std::uint64_t now_ms);

    ecs::World* world_;
    WireSchema schema_;
    NetIdAllocator allocator_;
    NetIdMap map_;
    ecs::ComponentId replicated_id_{};
    std::vector<ClientState> clients_;

    // Reused across ticks so the steady state allocates nothing.
    std::vector<std::byte> scratch_;
    std::vector<std::vector<std::byte>> records_;
    std::vector<float> record_priority_;        // parallel to records_, for the ordering pass
    std::vector<std::uint32_t> record_slot_;    // parallel to records_: whose record each one is
    std::vector<std::uint8_t> record_entry_;    // parallel to records_: was this an entry send?
    std::vector<std::uint8_t> produced_record_; // NetId::index → did this slot produce a record?
    // NetId::index → did the ENTRY PASS already serialize this slot in full this tick? The chunk
    // walk skips those rows: the entry record carries every replicable column, so anything the
    // version delta would add is a subset of what already went out.
    std::vector<std::uint8_t> entry_emitted_;
    // Scratch for the entry pass's per-entity column list, reused across entities and ticks.
    std::vector<std::pair<ecs::ComponentId, WireComponentId>> entry_columns_;
    std::vector<net::Received> inbox_;

    std::uint64_t delta_packets_sent_ = 0;
    std::uint64_t multipart_ticks_ = 0;
    std::uint64_t full_reseeds_ = 0;
    std::uint64_t entities_over_budget_ = 0;
    std::uint64_t entities_culled_ = 0;
    std::uint64_t entities_entered_ = 0;
    std::uint64_t entry_pass_records_ = 0;
    std::uint64_t delta_ticks_ = 0;

    RelevancyFn relevancy_;
    Budget budget_;

    // Per-tick scratch for the relevancy call, reused across clients and ticks.
    std::vector<ecs::Entity> candidates_;
    std::vector<float> priorities_;
    // NetId::index → this client's priority for it, or 0 when the policy said nothing.
    std::vector<float> priority_by_index_;
};

} // namespace rime::replication
