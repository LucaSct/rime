// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/server_replicator.hpp"

#include <algorithm>
#include <numeric>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/reflect/serialize.hpp"
#include "rime/ecs/archetype.hpp"

namespace rime::replication {
namespace {

void write_net_id(core::ByteWriter& w, NetId id) {
    w.u32(id.index);
    w.u32(id.generation);
}

} // namespace

ServerReplicator::ServerReplicator(ecs::World& world)
    : world_(&world), schema_(WireSchema::build(world.components())) {
    // Registering the tag here (rather than requiring the game to) keeps the marker an
    // implementation detail of this module. It is unreflected, so it costs nothing in the schema
    // hash or the wire schema.
    replicated_id_ = world.register_component<Replicated>();
}

NetId ServerReplicator::replicate(ecs::Entity entity) {
    if (!world_->is_alive(entity)) {
        return kNullNetId;
    }
    if (const auto existing = map_.net_id_of(entity)) {
        return *existing; // idempotent: replicating twice is a no-op, not a second identity
    }
    const NetId id = allocator_.allocate();
    map_.bind(id, entity);
    (void)world_->add_component_raw(entity, replicated_id_);
    return id;
}

void ServerReplicator::despawn(ecs::Entity entity) {
    if (const auto id = map_.net_id_of(entity)) {
        // Order matters: retract the identity first, so the per-client diff in publish_structure
        // sees the id as no-longer-live and emits a Despawn. Freeing the allocator slot also bumps
        // its generation, which is what makes a recycled index safe (net_id.hpp).
        map_.unbind(*id);
        allocator_.free(*id);

        // Forget every client's relevancy bit for this slot. The slot will be recycled with a
        // bumped generation, and `was_relevant` is a per-item record keyed by INDEX alone — so
        // without this the next tenant inherits the dead entity's "this client already has it", and
        // the entry send that a newly-relevant entity depends on is skipped for an entity that was
        // never sent.
        //
        // Corollary 2 of docs/design/replication.md: a per-peer record of what a client holds may
        // only ever strengthen on confirmed holding. Inheriting a bit across a recycle is
        // strengthening on nothing at all. Today the mistake is masked — a freshly spawned entity
        // always has a fresh column write, so it rides out on the ordinary changed-since-baseline
        // path rather than needing the entry path — but that is a coincidence of spawn order, not a
        // guarantee, and it is exactly the kind of coincidence the previous five instances relied
        // on.
        for (ClientState& state : clients_) {
            if (state.in_use && id->index < state.was_relevant.size()) {
                state.was_relevant[id->index] = 0;
            }
            // Same rule, same reason: a recycled slot must not inherit the dead entity's
            // accumulated starvation and jump the queue on its behalf.
            if (state.in_use && id->index < state.starved_ticks.size()) {
                state.starved_ticks[id->index] = 0;
            }
        }
    }
    (void)world_->despawn(entity);
}

void ServerReplicator::on_session_events(const std::vector<net::SessionEvent>& events) {
    for (const net::SessionEvent& event : events) {
        switch (event.kind) {
            case net::SessionEvent::Kind::Connected:
                // A fresh ClientState: nothing announced, baseline 0. Baseline 0 is not a special
                // case anywhere — every column stamp is > 0 (World::version() starts at 1), so the
                // ordinary delta path naturally sends this client the entire world.
                client_for(event.id);
                break;
            case net::SessionEvent::Kind::Disconnected:
            case net::SessionEvent::Kind::ConnectFailed:
                if (ClientState* state = find_client(event.id)) {
                    state->in_use = false;
                    state->announced.clear();
                    state->announced.shrink_to_fit();
                }
                break;
        }
    }
}

ServerReplicator::ClientState* ServerReplicator::find_client(net::SessionId id) noexcept {
    for (ClientState& state : clients_) {
        if (state.in_use && state.id == id) {
            return &state;
        }
    }
    return nullptr;
}

ServerReplicator::ClientState& ServerReplicator::client_for(net::SessionId id) {
    if (ClientState* existing = find_client(id)) {
        return *existing;
    }
    // Reset by assigning a default-constructed state rather than a positional aggregate. Every
    // per-client field must start clean when a slot is reused — a stale one is inherited by a
    // different peer entirely — and a positional list silently stops covering new members as they
    // are added, which is the same "a record outlived its subject" hazard one level up.
    const auto fresh = [id]() {
        ClientState state{};
        state.id = id;
        state.in_use = true;
        return state;
    };
    for (ClientState& state : clients_) {
        if (!state.in_use) {
            state = fresh();
            return state;
        }
    }
    clients_.push_back(fresh());
    return clients_.back();
}

ecs::Version ServerReplicator::complete_through(net::SessionId id) const noexcept {
    for (const ClientState& state : clients_) {
        if (state.in_use && state.id == id) {
            return state.complete_through;
        }
    }
    return 0;
}

ecs::Version ServerReplicator::acked_baseline(net::SessionId id) const noexcept {
    for (const ClientState& state : clients_) {
        if (state.in_use && state.id == id) {
            return state.acked_baseline;
        }
    }
    return 0;
}

std::size_t ServerReplicator::apply_inbound(net::NetDriver& driver) {
    std::size_t acks = 0;
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr) {
            continue;
        }
        inbox_.clear();
        (void)session->drain_received(inbox_);
        acks += apply_messages(id, inbox_);
    }
    return acks;
}

std::size_t ServerReplicator::apply_messages(net::SessionId id,
                                             std::span<const net::Received> messages) {
    std::size_t acks = 0;
    for (const net::Received& message : messages) {
        core::ByteReader reader{message.bytes};
        std::uint8_t tag = 0;
        std::uint64_t watermark = 0;
        if (!reader.u8(tag) || tag != static_cast<std::uint8_t>(MessageTag::BaselineAck) ||
            !reader.u64(watermark)) {
            continue; // another module's tag, or truncated — the bounds-checked reader made it
                      // harmless either way
        }
        ClientState& state = client_for(id);
        // Monotonic only. An ack can arrive reordered behind a newer one (they ride the
        // unreliable channel), and letting the baseline go BACKWARDS would re-send state the
        // client already has — wasteful, but worse, it would make the baseline stop being a
        // watermark and start being "whatever arrived last".
        state.acked_baseline = std::max(state.acked_baseline, watermark);
        ++acks;
    }
    return acks;
}

void ServerReplicator::set_relevancy(RelevancyFn fn) {
    relevancy_ = std::move(fn);
}

void ServerReplicator::set_budget(const Budget& budget) {
    budget_ = budget;
}

void ServerReplicator::publish(net::NetDriver& driver, std::uint64_t now_ms) {
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr || session->state() != net::SessionState::Connected) {
            continue;
        }
        ClientState& state = client_for(id);
        publish_structure(*session, state, now_ms);
        publish_delta(*session, state, now_ms);
    }
}

void ServerReplicator::publish_structure(net::Session& session,
                                         ClientState& state,
                                         std::uint64_t now_ms) {
    // Diff what this client has been told against what is actually live. This is deliberately a
    // full diff rather than an event queue: an event queue has to be repaired when a message is
    // refused under backpressure, whereas a diff simply produces the same answer again next tick.
    // Self-healing beats correct-if-nothing-goes-wrong.
    const std::size_t slots = std::max(state.announced.size(), allocator_.slot_count());
    state.announced.resize(slots, 0);

    // Each entry carries the value `announced[index]` held BEFORE this tick's diff touched it. The
    // rollback below restores that, rather than a constant chosen from the message kind — see there
    // for the bug that distinction fixes.
    std::vector<std::pair<NetId, std::uint32_t>> to_spawn;
    std::vector<std::pair<NetId, std::uint32_t>> to_despawn;
    for (std::size_t i = 0; i < slots; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const std::uint32_t announced = state.announced[i];
        const NetId live = allocator_.live_id_at(index);

        if (live.is_valid() && announced != live.generation) {
            if (announced != 0) {
                // The index was recycled without this client hearing about the death. Retract the
                // old incarnation before announcing the new one, or the client would rebind an
                // index it still thinks holds something else.
                to_despawn.emplace_back(NetId{index, announced}, announced);
            }
            to_spawn.emplace_back(live, announced);
            state.announced[i] = live.generation;
        } else if (!live.is_valid() && announced != 0) {
            to_despawn.emplace_back(NetId{index, announced}, announced);
            state.announced[i] = 0;
        }
    }

    // Despawns before spawns: within one tick an index can legitimately do both (recycled), and the
    // client must drop the old mirror before binding the new one to the same slot.
    const auto flush = [&](std::vector<std::pair<NetId, std::uint32_t>>& ids, MessageTag tag) {
        std::size_t sent = 0;
        while (sent < ids.size()) {
            // 9 bytes of header, 8 per id — pack as many as the payload allows.
            const std::size_t room = (kMaxReplicationPayload - 3) / 8;
            const std::size_t n = std::min(room, ids.size() - sent);
            scratch_.clear();
            core::ByteWriter writer{scratch_};
            writer.u8(static_cast<std::uint8_t>(tag));
            writer.u16(static_cast<std::uint16_t>(n));
            for (std::size_t i = 0; i < n; ++i) {
                write_net_id(writer, ids[sent + i].first);
            }
            if (!session.send_reliable(scratch_, now_ms)) {
                // Backpressure (the peer is drowning, or the 256-message backlog is full). Stop,
                // and roll the unsent entries back so next tick's diff re-emits them.
                //
                // RESTORE THE PRE-TICK VALUE, not a constant derived from the message kind. A
                // RECYCLED index appears in BOTH lists in one tick — {idx, old_gen} to despawn and
                // {idx, new_gen} to spawn, the case the comment above this lambda calls out. The
                // two flushes run back to back with no chance for the channel's backlog to drain
                // between them, so once the despawn flush hits backpressure the spawn flush is
                // certain to as well. A kind-derived rollback then has the spawn's `= 0` overwrite
                // the despawn's correctly-restored `old_gen`, and next tick's diff reads announced
                // == 0, takes the `announced != 0` branch as false, and NEVER RE-EMITS THE DESPAWN.
                // The server has permanently forgotten it owes one: the client rebinds the index to
                // the new incarnation and its old mirror entity is orphaned in the ECS world
                // forever, which is precisely the phantom this class's despawn() doc says it exists
                // to prevent.
                //
                // The pre-tick value is ground truth and is correct however the two flushes fail.
                // It can re-emit a despawn the client already applied — harmless, since a despawn
                // for an unbound id is a no-op — which is the self-healing direction this whole
                // diff is built on.
                for (std::size_t i = sent; i < ids.size(); ++i) {
                    const auto idx = static_cast<std::size_t>(ids[i].first.index);
                    state.announced[idx] = ids[i].second;
                }
                return;
            }
            sent += n;
        }
    };
    flush(to_despawn, MessageTag::Despawn);
    flush(to_spawn, MessageTag::Spawn);
}

void ServerReplicator::publish_delta(net::Session& session,
                                     ClientState& state,
                                     std::uint64_t now_ms) {
    const ecs::Version now_version = world_->version();

    // The stale-baseline valve. Once a client falls far enough behind, "changed since baseline"
    // approaches "everything" and we would pay that full cost every tick it stays behind. Re-seed
    // instead: reset to baseline 0 (which is the same full-world path a new client takes) and count
    // it, so a proof or a dashboard can see this happening rather than infer it from bandwidth.
    if (state.acked_baseline != 0 && now_version > state.acked_baseline + kStaleBaselineTicks) {
        state.acked_baseline = 0;
        ++full_reseeds_;
    }
    // THE EFFECTIVE BASELINE, and why it is not simply what the client acked.
    //
    // The client's acknowledgement is honest: it received and applied every part the server sent.
    // But the server may deliberately have sent only part of what the tick owed — over the packet
    // budget here, and once m11.5's relevancy lands, on purpose and every tick. Trusting the ack
    // alone advances the baseline past writes that were never transmitted, and an entity that then
    // stops changing is never re-offered: it stays wrong on that client forever. Measured before
    // the fix: 400 entities written once, 176 delivered, 224 permanently missing over a LOSSLESS
    // link.
    //
    // So the baseline is clamped to the newest tick this client was actually sent in full. Same
    // rule as the client's AckTracker, applied at the other end of the wire: only a COMPLETE tick
    // may advance a watermark.
    const ecs::Version baseline = std::min(state.acked_baseline, state.complete_through);

    // ── Relevancy: one policy call for this client, covering every replicated entity ────────────
    //
    // The result is splayed into `priority_by_index_` so the record walk below can answer "does
    // this client care, and how much" with an array read instead of a call. With no policy
    // installed every entity scores 1.0, which is the m11.3/11.4 behaviour exactly.
    // A slot that names no live entity scores 0 — NOT the 1.0 default a live unscored entity gets.
    // The allocator's slot vector never shrinks, so after any despawn it holds indices that name
    // nothing, and `map_.for_each` (and therefore the policy) never visits them. Defaulting those
    // to "relevant" made them read as entities eternally about to enter, which pinned the full-walk
    // trigger below on forever: a permanent, silent, per-client cost with byte-identical output.
    // Debris are culled and despawned continuously, so distance relevancy makes that the steady
    // state rather than an edge case. Measured before the fix: 30 full walks in 30 quiet ticks.
    const std::size_t slot_count = allocator_.slot_count();
    priority_by_index_.assign(slot_count, 0.0f);
    map_.for_each([this](NetId id, ecs::Entity) {
        if (id.index < priority_by_index_.size()) {
            priority_by_index_[id.index] = 1.0f;
        }
    });
    if (relevancy_) {
        candidates_.clear();
        map_.for_each([this](NetId, ecs::Entity entity) { candidates_.push_back(entity); });
        priorities_.assign(candidates_.size(), 0.0f);
        relevancy_(state.id, candidates_, priorities_);
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            if (const auto id = map_.net_id_of(candidates_[i])) {
                if (id->index < priority_by_index_.size()) {
                    priority_by_index_[id->index] = priorities_[i];
                }
            }
        }
    }
    state.was_relevant.resize(slot_count, 0);
    state.starved_ticks.resize(slot_count, 0);

    ++delta_ticks_;

    // ── Relevancy bookkeeping, applied at every exit from this function ──────────────────────────
    //
    // `was_relevant` is a per-item claim about what this client HOLDS, so corollary 2 governs it:
    // it may strengthen only on evidence about that entity's own transmission, never on "we built a
    // record for it". Those differ whenever the byte budget bites, and the gap is not benign — an
    // ENTERING entity is by definition one whose state has not changed since the baseline, so if
    // its entry record is dropped and we mark it delivered anyway, the ordinary change test will
    // never re-offer it and the client mirrors it empty forever. That is the m11.5 foundation bug
    // again, one layer up.
    //
    // So the rule, by case:
    //   irrelevant                     → 0. It is not owed anything.
    //   relevant, nothing to send      → 1. Vacuously satisfied; this is also the ONLY thing that
    //                                    marks an entity with no replicable columns at all, which
    //                                    would otherwise read as eternally entering and pin the
    //                                    full walk on forever (instance six's shape).
    //   relevant, record actually sent → 1. Real evidence.
    //   relevant, record dropped       → unchanged. An entry stays 0 and retries next tick; an
    //                                    ordinary change stays 1, because the client still holds
    //                                    the older value and the clamped baseline will re-offer it.
    //
    // Split across two helpers because the evidence arrives at two different moments. `credit_sent`
    // runs per PART as each one is accepted; `settle_relevancy` runs once afterwards and handles
    // everything no packet spoke for.

    // The "record actually sent" case. Called per part, and only for a part the session ACCEPTED —
    // `send_unreliable` refuses on a non-Connected session or a channel that will not take the
    // datagram, and a refused part is bytes that never existed. Crediting the whole prefix
    // regardless would strengthen the record on "we tried to send it", which is the
    // weaker-correlated event the invariant names by that exact phrase.
    const auto credit_sent = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end && i < record_slot_.size(); ++i) {
            const std::uint32_t slot = record_slot_[i];
            if (slot < state.was_relevant.size()) {
                state.was_relevant[slot] = 1;
            }
            if (slot < state.starved_ticks.size()) {
                state.starved_ticks[slot] = 0; // paid
            }
            if (record_entry_[i] != 0) {
                ++entities_entered_;
            }
        }
    };

    // Refuse a record that cannot fit in a packet on its own. An entity's record is never split
    // (parts are independently-complete packets, not fragments), so such a record would be built,
    // packed alone into an oversized part, and refused by the channel every tick forever — losing
    // the entity silently or jamming `complete_through`, depending on how honestly completeness is
    // judged. Neither is a thing to do quietly. Dropping it here keeps the rest of the world
    // converging, and `records_too_large()` names the real fault: a component that needs splitting.
    const auto record_fits = [](std::size_t record_bytes) {
        return kHeaderBytes + record_bytes <= kMaxReplicationPayload;
    };

    // The ordering key: the policy's priority plus what this client is owed. See
    // Budget::starvation_gain — being passed over is itself a claim on the next tick's budget,
    // which is what stops a strict ordering from becoming a starvation machine.
    const auto aged_priority = [&](std::size_t slot, float priority) {
        const std::uint32_t age =
            slot < state.starved_ticks.size() ? state.starved_ticks[slot] : 0u;
        return priority + budget_.starvation_gain * static_cast<float>(age);
    };

    // Record that a built record did NOT go out, so it outranks its peers next tick.
    const auto bump_starvation = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end && i < record_slot_.size(); ++i) {
            const std::uint32_t slot = record_slot_[i];
            if (slot < state.starved_ticks.size()) {
                ++state.starved_ticks[slot];
            }
        }
    };

    // The other two cases: anything irrelevant is cleared, and anything relevant that produced no
    // record at all is marked held — vacuously, because nothing was owed. Run at every exit from
    // this function, including the early ones, or an entity with nothing to send never gets its bit
    // and reads as eternally entering.
    const auto settle_relevancy = [&]() {
        for (std::size_t slot = 0; slot < slot_count; ++slot) {
            if (!(priority_by_index_[slot] > 0.0f)) {
                state.was_relevant[slot] = 0;
            } else if (slot >= produced_record_.size() || produced_record_[slot] == 0) {
                state.was_relevant[slot] = 1;
            }
        }
    };

    // ── Collect one record per entity with at least one changed replicable component ────────────
    records_.clear();
    record_priority_.clear();
    record_slot_.clear();
    record_entry_.clear();
    produced_record_.assign(slot_count, 0);
    entry_emitted_.assign(slot_count, 0);

    // ── The entry pass ───────────────────────────────────────────────────────────────────────────
    //
    // An entity ENTERING a client's relevant set has, by definition, not changed since that
    // client's baseline — it was simply never sent — so the chunk walk's "changed since baseline"
    // test cannot see it. Something has to send it anyway.
    //
    // The obvious fix, and the one this replaced, was a single global flag: if anything anywhere
    // was entering, give up the skip and re-examine every column of every chunk of every replicated
    // archetype for that client. The cost of that is proportional to the size of the WHOLE
    // replicated world and it is triggered by ONE transition anywhere in it — so a static
    // 10,000-entity level got walked in full because a single distant chunk of rubble drifted into
    // range. The comment defending it said transitions are "rare and bursty". At the ADR's target —
    // 64 clients, ~1000 debris, debris moving, viewpoints moving — the chance that *some* pair
    // crosses *some* radius on a given tick approaches 1, so the flag is stuck on and the
    // chunk-version skip, which the header calls the whole reason this design needs no history
    // buffer, is effectively off whenever relevancy is on.
    //
    // So: emit the entering entities directly, and leave the chunk walk alone. This loop visits
    // only the candidates already being iterated for relevancy, serializes only the ones actually
    // entering, and pushes into the same record arrays — everything downstream (sort, rotate,
    // budget, packetize, credit_sent) is agnostic about which pass produced a record. Cost becomes
    // O(what changed) + O(what is entering for this client), with no coupling to how much of the
    // rest of the world exists. The "rare and bursty" assumption is not merely better founded now;
    // it is unnecessary.
    map_.for_each([&](NetId id, ecs::Entity entity) {
        const std::size_t slot = id.index;
        if (slot >= slot_count || !(priority_by_index_[slot] > 0.0f) ||
            state.was_relevant[slot] != 0) {
            return; // dead, irrelevant, or already held — none of them are entering
        }

        // Every replicable column this entity has, not just the changed ones: the client has none
        // of its state, so a delta against a baseline it never had would be meaningless.
        entry_columns_.clear();
        for (const ecs::ComponentId local : world_->signature_of(entity).ids()) {
            const WireComponentId wire = schema_.wire_id_of(local);
            if (wire != kInvalidWireComponentId) {
                entry_columns_.emplace_back(local, wire);
            }
        }
        if (entry_columns_.empty()) {
            return; // nothing replicable to send; settle_relevancy marks it held vacuously
        }

        scratch_.clear();
        core::ByteWriter writer{scratch_};
        write_net_id(writer, id);
        writer.u8(static_cast<std::uint8_t>(entry_columns_.size()));
        for (const auto& [local, wire] : entry_columns_) {
            writer.u16(static_cast<std::uint16_t>(wire));
            const void* value = world_->get_component_raw(entity, local);
            const core::TypeInfo* type = nullptr;
            ecs::ComponentId unused{};
            std::size_t packed = 0;
            (void)schema_.lookup(wire, unused, type, packed);
            const std::vector<std::byte> bytes = core::serialize(*type, value);
            writer.bytes(bytes);
        }
        if (!record_fits(scratch_.size())) {
            ++records_too_large_;
            return;
        }
        records_.push_back(scratch_);
        record_priority_.push_back(aged_priority(slot, priority_by_index_[slot]));
        record_slot_.push_back(static_cast<std::uint32_t>(slot));
        record_entry_.push_back(1);
        produced_record_[slot] = 1;
        entry_emitted_[slot] = 1;
        ++entry_pass_records_;
    });

    const std::size_t archetypes = world_->archetype_count();
    for (std::size_t ai = 0; ai < archetypes; ++ai) {
        ecs::Archetype& arch = world_->archetype(ai);
        if (!arch.signature().contains(replicated_id_)) {
            continue;
        }
        const ecs::ChunkLayout& layout = arch.layout();

        // Which of this archetype's columns are replicable at all — computed once per archetype
        // rather than per chunk or per row.
        std::vector<std::pair<ecs::ComponentId, WireComponentId>> replicable;
        for (const ecs::ColumnLayout& column : layout.columns) {
            const WireComponentId wire = schema_.wire_id_of(column.id);
            if (wire != kInvalidWireComponentId) {
                replicable.emplace_back(column.id, wire);
            }
        }
        if (replicable.empty()) {
            continue;
        }

        const auto chunk_count = static_cast<std::uint32_t>(arch.chunk_count());
        for (std::uint32_t ci = 0; ci < chunk_count; ++ci) {
            ecs::Chunk& chunk = arch.chunk(ci);

            // The per-chunk-per-column skip test — the whole reason this design needs no history
            // buffer. A column untouched since the client's baseline contributes nothing.
            //
            // Unconditional again, as it was before relevancy existed. The entry pass above handles
            // the one case this test structurally cannot see (an entity that is newly relevant and
            // therefore unchanged), so nothing has to widen it any more.
            std::vector<std::pair<ecs::ComponentId, WireComponentId>> dirty;
            for (const auto& [local, wire] : replicable) {
                if (chunk.column_version(local) > baseline) {
                    dirty.emplace_back(local, wire);
                }
            }
            if (dirty.empty()) {
                continue;
            }

            const std::uint32_t rows = chunk.size();
            for (std::uint32_t row = 0; row < rows; ++row) {
                const ecs::Entity entity = chunk.entity_at(row);
                const auto net_id = map_.net_id_of(entity);
                if (!net_id) {
                    continue; // replicated tag but no identity yet — nothing to name it by
                }
                const std::uint32_t slot = net_id->index;
                const float priority =
                    slot < priority_by_index_.size() ? priority_by_index_[slot] : 1.0f;
                if (!(priority > 0.0f)) {
                    ++entities_culled_;
                    continue; // this client does not care about it right now
                }
                if (slot < entry_emitted_.size() && entry_emitted_[slot] != 0) {
                    // The entry pass already serialized this entity's FULL state this tick. The
                    // dirty columns here are a subset of what went out, so a second record would be
                    // redundant bytes competing for the same budget.
                    continue;
                }
                // Note what is NOT done here: `was_relevant` is not set and nothing is counted as
                // delivered. Both wait until the record is known to have survived the byte budget
                // AND been accepted by the session — see `credit_sent`.
                scratch_.clear();
                core::ByteWriter writer{scratch_};
                write_net_id(writer, *net_id);
                writer.u8(static_cast<std::uint8_t>(dirty.size()));
                for (const auto& [local, wire] : dirty) {
                    writer.u16(static_cast<std::uint16_t>(wire));
                    const void* value = chunk.component(local, row);
                    const core::TypeInfo* type = nullptr;
                    ecs::ComponentId unused{};
                    std::size_t packed = 0;
                    (void)schema_.lookup(wire, unused, type, packed);
                    const std::vector<std::byte> bytes = core::serialize(*type, value);
                    writer.bytes(bytes);
                }
                if (!record_fits(scratch_.size())) {
                    ++records_too_large_;
                    continue;
                }
                records_.push_back(scratch_);
                record_priority_.push_back(aged_priority(slot, priority));
                record_slot_.push_back(slot);
                // Never an entry: by the time this walk runs, every entering slot has already been
                // emitted above and is skipped here.
                record_entry_.push_back(0u);
                if (slot < produced_record_.size()) {
                    produced_record_[slot] = 1;
                }
            }
        }
    }

    if (records_.empty()) {
        settle_relevancy();
        return;
    }

    // Resume where the last tick stopped. A world permanently over budget otherwise re-sends the
    // same prefix of an identically-ordered candidate list every tick, and everything past the
    // cut-off is never delivered at all — the baseline clamp above makes the ACK honest, and this
    // makes DELIVERY happen. Rotating the list rather than tracking an offset through the packing
    // loop keeps the loop itself unchanged, and the cost is a move per record on a path that is
    // already allocating them.
    if (relevancy_) {
        // Nearest-first (or whatever the policy means by "first"). A STABLE sort, so entities the
        // policy scored equally keep the archetype walk's order — which is what lets the rotation
        // cursor below still make progress through a large equal-priority tail instead of
        // reshuffling it every tick and starving the same entities repeatedly.
        std::vector<std::size_t> order(records_.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
            return record_priority_[a] > record_priority_[b];
        });
        std::vector<std::vector<std::byte>> sorted;
        std::vector<std::uint32_t> sorted_slots;
        std::vector<std::uint8_t> sorted_entries;
        sorted.reserve(records_.size());
        sorted_slots.reserve(records_.size());
        sorted_entries.reserve(records_.size());
        for (const std::size_t i : order) {
            sorted.push_back(std::move(records_[i]));
            // The parallel arrays must ride along, or `settle_relevancy` credits delivery to
            // whichever entity happens to sit at that position after the sort — marking one entity
            // as held on the strength of a different entity's packet.
            sorted_slots.push_back(record_slot_[i]);
            sorted_entries.push_back(record_entry_[i]);
        }
        records_.swap(sorted);
        record_slot_.swap(sorted_slots);
        record_entry_.swap(sorted_entries);
    } else if (state.cursor != 0 && state.cursor < records_.size()) {
        // No policy: no meaningful order, so rotation is the only fairness there is.
        const auto offset = static_cast<std::ptrdiff_t>(state.cursor);
        std::rotate(records_.begin(), records_.begin() + offset, records_.end());
        std::rotate(record_slot_.begin(), record_slot_.begin() + offset, record_slot_.end());
        std::rotate(record_entry_.begin(), record_entry_.begin() + offset, record_entry_.end());
    } else {
        state.cursor = 0;
    }

    // The byte budget, applied AFTER ordering so a tight budget keeps what the policy said mattered
    // most.
    //
    // Whatever it drops must be remembered, not just discarded. Trimming `records_` makes the tick
    // LOOK complete to the packet loop below — every surviving record fits, so `covered ==
    // records_.size()` — and the completeness test would then advance `complete_through`, which is
    // the client's baseline clamp. Advancing it past records the budget deliberately withheld is
    // corollary 1 exactly: a watermark moved on a tick that was only partly sent. The packet budget
    // already got this right; the byte budget, which is the one m11.5 actually turns on, did not.
    std::size_t dropped_by_bytes = 0;
    if (budget_.max_bytes_per_tick != 0) {
        std::size_t used = 0;
        std::size_t kept = 0;
        for (; kept < records_.size(); ++kept) {
            const std::size_t next = used + records_[kept].size();
            if (next > budget_.max_bytes_per_tick && kept > 0) {
                break;
            }
            used = next;
        }
        if (kept < records_.size()) {
            dropped_by_bytes = records_.size() - kept;
            entities_over_budget_ += dropped_by_bytes;
            bump_starvation(kept, records_.size()); // before the trim discards their slots
            records_.resize(kept);
            record_slot_.resize(kept);
            record_entry_.resize(kept);
        }
    }

    // ── Pack records into independently-complete packets ────────────────────────────────────────
    //
    // Not fragments of one logical packet — full packets, each valid on its own. Losing one costs
    // that packet's entities for that tick, not the whole tick: with k fragments needing all k to
    // arrive, per-tick delivery would decay as (1-p)^k, which is the wrong way to spend an
    // unreliable channel. An entity's record is never split, so a torn tick means "some entities
    // are fresher than others" — which is what a snapshot stream already does under ordinary loss.
    constexpr std::size_t kHeader = kHeaderBytes;
    std::vector<std::pair<std::size_t, std::size_t>> parts; // [begin, end) into records_
    std::size_t begin = 0;
    std::size_t used = kHeader;
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const std::size_t size = records_[i].size();
        if (used + size > kMaxReplicationPayload && i > begin) {
            parts.emplace_back(begin, i);
            begin = i;
            used = kHeader;
            if (parts.size() >= kMaxDeltaPartsPerTick) {
                break;
            }
        }
        used += size;
    }
    if (parts.size() < kMaxDeltaPartsPerTick && begin < records_.size()) {
        parts.emplace_back(begin, records_.size());
    }
    if (parts.empty()) {
        settle_relevancy();
        return;
    }

    const auto part_count = static_cast<std::uint8_t>(parts.size());
    if (part_count > 1) {
        ++multipart_ticks_;
    }
    bool every_part_sent = true;
    for (std::size_t p = 0; p < parts.size(); ++p) {
        scratch_.clear();
        core::ByteWriter writer{scratch_};
        writer.u8(static_cast<std::uint8_t>(MessageTag::Delta));
        writer.u64(now_version);
        writer.u8(static_cast<std::uint8_t>(p));
        writer.u8(part_count);
        writer.u16(static_cast<std::uint16_t>(parts[p].second - parts[p].first));
        for (std::size_t i = parts[p].first; i < parts[p].second; ++i) {
            writer.bytes(records_[i]);
        }
        if (session.send_unreliable(scratch_, now_ms)) {
            ++delta_packets_sent_;
            // Credited HERE, per part, and only on acceptance. A refused part is bytes that never
            // existed; crediting the whole prefix regardless would strengthen a per-item record on
            // "we tried to send it", which is the weaker-correlated event the invariant names.
            credit_sent(parts[p].first, parts[p].second);
        } else {
            // Refused by the channel. These records did not reach the wire, so they age exactly
            // like budget-dropped ones — the rule is "a record that was not delivered is owed",
            // and it must not depend on WHICH stage declined to carry it.
            bump_starvation(parts[p].first, parts[p].second);
            every_part_sent = false;
        }
    }
    settle_relevancy();

    const std::size_t covered = parts.back().second;
    if (covered < records_.size() || dropped_by_bytes > 0 || !every_part_sent) {
        // Over one of the two budgets. The remainder waits — and genuinely does arrive, because the
        // baseline clamp above keeps it in the candidate set and the cursor below makes the next
        // tick resume here rather than restart. Latency, not loss; the two mechanisms are what earn
        // that claim, which the code made without them until m11.5 checked it.
        //
        // Note the byte-budget drops were already counted where they happened; only the
        // packet-count remainder is added here, or a record refused by both budgets would be
        // counted twice.
        entities_over_budget_ += records_.size() - covered;
        bump_starvation(covered, records_.size());
        state.cursor += covered;
    } else {
        // Everything owed was sent AND every part was accepted, so this tick may advance the
        // delivery watermark — and the next one starts from the top again. A refused part makes the
        // tick partial in exactly the sense corollary 1 cares about, however healthy the link is.
        state.complete_through = now_version;
        state.cursor = 0;
    }
}

} // namespace rime::replication
