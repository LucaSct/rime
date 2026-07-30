// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/server_replicator.hpp"

#include <algorithm>

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
            state = ClientState{id, true, 0, {}};
            return state;
        }
    }
    clients_.push_back(ClientState{id, true, 0, {}});
    return clients_.back();
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

    std::vector<NetId> to_spawn;
    std::vector<NetId> to_despawn;
    for (std::size_t i = 0; i < slots; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const std::uint32_t announced = state.announced[i];
        const NetId live = allocator_.live_id_at(index);

        if (live.is_valid() && announced != live.generation) {
            if (announced != 0) {
                // The index was recycled without this client hearing about the death. Retract the
                // old incarnation before announcing the new one, or the client would rebind an
                // index it still thinks holds something else.
                to_despawn.push_back(NetId{index, announced});
            }
            to_spawn.push_back(live);
            state.announced[i] = live.generation;
        } else if (!live.is_valid() && announced != 0) {
            to_despawn.push_back(NetId{index, announced});
            state.announced[i] = 0;
        }
    }

    // Despawns before spawns: within one tick an index can legitimately do both (recycled), and the
    // client must drop the old mirror before binding the new one to the same slot.
    const auto flush = [&](std::vector<NetId>& ids, MessageTag tag) {
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
                write_net_id(writer, ids[sent + i]);
            }
            if (!session.send_reliable(scratch_, now_ms)) {
                // Backpressure (the peer is drowning, or the 256-message backlog is full). Stop —
                // the ids we did not send stay un-announced in `state.announced`… except we
                // already marked them. Roll the unsent ones back so next tick's diff re-emits them.
                for (std::size_t i = sent; i < ids.size(); ++i) {
                    const auto idx = static_cast<std::size_t>(ids[i].index);
                    state.announced[idx] = tag == MessageTag::Spawn ? 0 : ids[i].generation;
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
    const ecs::Version baseline = state.acked_baseline;

    // ── Collect one record per entity with at least one changed replicable component ────────────
    records_.clear();
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
            }
        }
    }

    if (records_.empty()) {
        return;
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
        // Over the per-tick packet budget. The remainder is simply not sent: the baseline mechanism
        // re-offers it next tick, so this is latency, never loss. It is also the signal that the
        // world has outgrown unprioritized broadcast — which is exactly what m11.5 exists to fix,
        // and deliberately not something to solve here with an ad-hoc priority heuristic.
        entities_over_budget_ += records_.size() - covered;
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
