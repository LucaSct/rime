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

    // Priority an entity gains for each tick it was owed to a client and not sent. Reset the moment
    // it is delivered.
    //
    // WITHOUT THIS, PRIORITY SILENTLY MEANS "SOME ENTITIES ARE NEVER SENT". A strict ordering plus
    // a budget that cannot cover the world is a starvation machine: the same highest-priority
    // prefix goes out every tick and everything below the cut-off is never delivered at all. Not
    // late — never. That is not a graceful degradation, it is a permanently wrong client, and it is
    // the exact liveness bug the rotation cursor was built to prevent on the un-prioritized path.
    //
    // Aging fixes it without giving up what priority is FOR. The ordering is still nearest-first
    // among entities equally owed; what changes is that being passed over is itself a claim on the
    // next tick's budget, so the tail rises rather than sinking. It also degrades in the right
    // direction: with a budget that comfortably covers the world nothing is ever starved, every age
    // is zero, and the ordering is exactly what the policy asked for.
    //
    // The gain sets how long a maximally-unimportant entity can be held back. Priorities from
    // `distance_relevancy` land in (0, 1] (2.0 for unpositioned), so at the default an entity
    // overtakes the most important thing in the world after ~40 ticks — well under a second at 60
    // Hz. Raise it to favour fairness over locality, lower it for the reverse. Zero disables aging
    // and restores strict priority, which is a defensible choice only if the budget is known to
    // cover the working set.
    float starvation_gain = 0.05f;
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
    // with nothing that ever repairs it.
    //
    // The discipline now has a BACKSTOP (m12.3) rather than only a comment — see
    // `net_ids_orphaned()`. It is still a discipline: the backstop repairs the mistake a tick late
    // and makes it loud, which is strictly better than a phantom and strictly worse than calling
    // this function.
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

    // NetIds retracted by the BACKSTOP rather than by `despawn` — i.e. entities that were killed
    // with a bare `world.despawn()` while still carrying a network identity (ADR-0035 §6 ruled this
    // to m12.3, as the first brick to add a new replication consumer).
    //
    // WHAT WENT WRONG, AND WHY A COUNTER IS THE RIGHT SHAPE OF FIX. `despawn` above is the only
    // path that retracts an id, so an entity destroyed behind the replicator's back leaves its
    // NetId live in the allocator and bound in the map forever. The per-client structure diff then
    // sees an id that is still live and never emits a Despawn: every client keeps a mirror of an
    // entity that no longer exists, permanently, with no message that repairs it. It is the exact
    // failure `AckTracker` and the relevancy-entry pass were each built to prevent, arriving
    // through a door neither of them watches.
    //
    // The backstop runs once per `publish` — not per client — and does three things: it RETRACTS
    // the id properly (so clients get the Despawn one tick late instead of never), it WARNS with
    // the offending NetId, and it counts. Repairing without counting would be worse than either,
    // because it would make the mistake invisible and therefore permanent in the source.
    //
    // The cost is one walk of the live-id slots per tick, comparing an entity handle against the
    // directory — the same order as the structure diff already does per client, so it is
    // strictly cheaper than the work it sits beside. It should read ZERO in any healthy game; a
    // non-zero value names a call site that must be changed to use `despawn`.
    [[nodiscard]] std::uint64_t net_ids_orphaned() const noexcept { return net_ids_orphaned_; }

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

    // Records discarded because one entity's state does not fit in a single packet.
    //
    // AN ENTITY'S RECORD IS NEVER SPLIT — that is the framing contract (see the packing loop: parts
    // are independently-complete packets, not fragments of one logical message). So an entity whose
    // serialized components exceed the payload budget on their own cannot be transmitted AT ALL, by
    // any amount of budget or patience. It is a schema problem, not a bandwidth problem: the fix is
    // to split the component or stop replicating it.
    //
    // What matters here is that the engine says so. The two ways of NOT saying so are both bad and
    // both were live: let the oversized part be built and refused by the channel, which either
    // silently loses the entity forever (if the tick still counts as complete) or jams
    // `complete_through` forever (if it does not, which is the honest reading). Dropping it at
    // build time and counting it keeps the rest of the world converging and makes the real fault
    // visible.
    //
    // Non-zero means a component needs splitting. It should be zero in any healthy game.
    [[nodiscard]] std::uint64_t records_too_large() const noexcept { return records_too_large_; }

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

        // Indexed by NetId::index: how many consecutive ticks this client has had a record built
        // for this entity that was then dropped by a budget. Reset to 0 the moment one is actually
        // sent. This is what `Budget::starvation_gain` multiplies.
        //
        // It is a per-item record keyed by a RECYCLABLE index, so it lives under the same rule as
        // `was_relevant` and is cleared in despawn() for the same reason — a recycled slot must not
        // inherit the dead entity's grievance and jump the queue on its behalf. Note this one is a
        // claim about what WE OWE rather than about what the peer holds, so it is not corollary 2
        // proper; the recycle hazard is identical regardless of which direction the claim points.
        std::vector<std::uint32_t> starved_ticks;
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

    // The `despawn` discipline backstop — see net_ids_orphaned(). Runs once per publish, before
    // any client is looked at, so every client sees the same repaired world in the same tick.
    void reap_orphaned_ids();

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
    std::uint64_t records_too_large_ = 0;
    std::uint64_t delta_ticks_ = 0;
    std::uint64_t net_ids_orphaned_ = 0;

    // Scratch for reap_orphaned_ids, reused across ticks: collecting the offenders before
    // retracting them keeps the walk from mutating the allocator it is iterating.
    std::vector<NetId> orphaned_;

    RelevancyFn relevancy_;
    Budget budget_;

    // Per-tick scratch for the relevancy call, reused across clients and ticks.
    std::vector<ecs::Entity> candidates_;
    std::vector<float> priorities_;
    // NetId::index → this client's priority for it, or 0 when the policy said nothing.
    std::vector<float> priority_by_index_;
};

} // namespace rime::replication
