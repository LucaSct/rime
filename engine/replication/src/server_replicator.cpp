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
    for (ClientState& state : clients_) {
        if (!state.in_use) {
            state = ClientState{id, true, 0, 0, 0, {}, {}};
            return state;
        }
    }
    clients_.push_back(ClientState{id, true, 0, 0, 0, {}, {}});
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
    const std::size_t slot_count = allocator_.slot_count();
    priority_by_index_.assign(slot_count, 1.0f);
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

    // Is anything ENTERING this client's relevant set this tick? It matters before the walk starts,
    // because the per-chunk "changed since baseline" skip below is what makes the delta cheap — and
    // an entering entity is precisely one that has NOT changed, so its chunk is clean and the skip
    // would step straight over it. On a tick with entries we give up the skip and walk everything;
    // transitions are rare and bursty, so paying a full pass on those ticks is the cheap trade
    // against a per-entity check that would slow down every tick instead.
    bool any_entering = false;
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        if (priority_by_index_[slot] > 0.0f && state.was_relevant[slot] == 0) {
            any_entering = true;
            break;
        }
    }

    // ── Collect one record per entity with at least one changed replicable component ────────────
    records_.clear();
    record_priority_.clear();
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
            std::vector<std::pair<ecs::ComponentId, WireComponentId>> dirty;
            for (const auto& [local, wire] : replicable) {
                if (any_entering || chunk.column_version(local) > baseline) {
                    dirty.emplace_back(local, wire);
                }
            }
            if (dirty.empty()) {
                continue;
            }

            // Whether this chunk genuinely changed since the baseline, as opposed to being walked
            // only because some OTHER entity is entering relevance this tick.
            bool chunk_changed = false;
            for (const auto& [local, wire] : replicable) {
                if (chunk.column_version(local) > baseline) {
                    chunk_changed = true;
                    break;
                }
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
                    if (slot < state.was_relevant.size()) {
                        state.was_relevant[slot] = 0;
                    }
                    ++entities_culled_;
                    continue; // this client does not care about it right now
                }
                const bool entering =
                    slot < state.was_relevant.size() && state.was_relevant[slot] == 0;
                if (any_entering && !entering && !chunk_changed) {
                    continue; // full-walk tick, but this row neither changed nor entered
                }
                if (entering) {
                    // Entering the relevant set. Its last write may predate the baseline — it was
                    // never sent, not "already known" — so it must go out regardless of version.
                    // Without this, an entity that comes into range while standing still is
                    // mirrored empty forever.
                    ++entities_entered_;
                    state.was_relevant[slot] = 1;
                }
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
                records_.push_back(scratch_);
                record_priority_.push_back(priority);
            }
        }
    }

    if (records_.empty()) {
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
        sorted.reserve(records_.size());
        for (const std::size_t i : order) {
            sorted.push_back(std::move(records_[i]));
        }
        records_.swap(sorted);
    } else if (state.cursor != 0 && state.cursor < records_.size()) {
        // No policy: no meaningful order, so rotation is the only fairness there is.
        std::rotate(records_.begin(),
                    records_.begin() + static_cast<std::ptrdiff_t>(state.cursor),
                    records_.end());
    } else {
        state.cursor = 0;
    }

    // The byte budget, applied AFTER ordering so a tight budget keeps what the policy said mattered
    // most. Trimming here rather than inside the packing loop keeps the over-budget bookkeeping in
    // one place: everything dropped is counted and the cursor advances past what was sent.
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
            records_.resize(kept);
        }
    }

    // ── Pack records into independently-complete packets ────────────────────────────────────────
    //
    // Not fragments of one logical packet — full packets, each valid on its own. Losing one costs
    // that packet's entities for that tick, not the whole tick: with k fragments needing all k to
    // arrive, per-tick delivery would decay as (1-p)^k, which is the wrong way to spend an
    // unreliable channel. An entity's record is never split, so a torn tick means "some entities
    // are fresher than others" — which is what a snapshot stream already does under ordinary loss.
    constexpr std::size_t kHeader = 1 + 8 + 1 + 1 + 2;
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
        return;
    }

    const std::size_t covered = parts.back().second;
    if (covered < records_.size()) {
        // Over the per-tick packet budget. The remainder waits — and genuinely does arrive, because
        // the baseline clamp above keeps it in the candidate set and the cursor below makes the
        // next tick resume here rather than restart. Latency, not loss; the two mechanisms are what
        // earn that claim, which the code made without them until m11.5 checked it.
        entities_over_budget_ += records_.size() - covered;
        state.cursor += covered;
    } else {
        // Everything owed was sent, so this tick may advance the delivery watermark — and the next
        // one starts from the top again.
        state.complete_through = now_version;
        state.cursor = 0;
    }

    const auto part_count = static_cast<std::uint8_t>(parts.size());
    if (part_count > 1) {
        ++multipart_ticks_;
    }
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
        }
    }
}

} // namespace rime::replication
