// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/client_replicator.hpp"

#include "rime/core/byte_cursor.hpp"
#include "rime/core/reflect/serialize.hpp"

namespace rime::replication {
namespace {

[[nodiscard]] bool read_net_id(core::ByteReader& reader, NetId& out) {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    if (!reader.u32(index) || !reader.u32(generation)) {
        return false;
    }
    out = NetId{index, generation};
    return true;
}

} // namespace

ClientReplicator::ClientReplicator(ecs::World& world)
    : world_(&world), schema_(WireSchema::build(world.components())) {
    replicated_id_ = world.register_component<Replicated>();
}

void ClientReplicator::apply_inbound(net::NetDriver& driver) {
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr) {
            continue;
        }
        inbox_.clear();
        (void)session->drain_received(inbox_);
        for (const net::Received& message : inbox_) {
            core::ByteReader reader{message.bytes};
            std::uint8_t tag = 0;
            if (!reader.u8(tag)) {
                ++malformed_;
                continue;
            }
            switch (static_cast<MessageTag>(tag)) {
                case MessageTag::Spawn:
                    on_spawn(reader);
                    break;
                case MessageTag::Despawn:
                    on_despawn(reader);
                    break;
                case MessageTag::Delta:
                    on_delta(reader);
                    break;
                default:
                    ++malformed_; // BaselineAck is client→server; anything else is not ours
                    break;
            }
        }
    }
}

void ClientReplicator::on_spawn(core::ByteReader& reader) {
    std::uint16_t count = 0;
    if (!reader.u16(count)) {
        ++malformed_;
        return;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        NetId id{};
        if (!read_net_id(reader, id)) {
            ++malformed_;
            return;
        }
        if (map_.resolve(id).is_valid()) {
            continue; // already bound — a re-announced spawn, which the diff on the server can
                      // legitimately produce. Idempotent, not an error.
        }
        const ecs::Entity local = world_->spawn();
        (void)world_->add_component_raw(local, replicated_id_);
        map_.bind(id, local);
        ++spawns_applied_;
    }
}

void ClientReplicator::on_despawn(core::ByteReader& reader) {
    std::uint16_t count = 0;
    if (!reader.u16(count)) {
        ++malformed_;
        return;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        NetId id{};
        if (!read_net_id(reader, id)) {
            ++malformed_;
            return;
        }
        const ecs::Entity local = map_.resolve(id);
        if (!local.is_valid()) {
            continue; // never had it, or a despawn for an incarnation already replaced
        }
        map_.unbind(id);
        (void)world_->despawn(local);
        ++despawns_applied_;
    }
}

void ClientReplicator::on_delta(core::ByteReader& reader) {
    std::uint64_t tick = 0;
    std::uint8_t part_index = 0;
    std::uint8_t part_count = 0;
    std::uint16_t entity_count = 0;
    if (!reader.u64(tick) || !reader.u8(part_index) || !reader.u8(part_count) ||
        !reader.u16(entity_count)) {
        ++malformed_;
        return;
    }

    for (std::uint16_t i = 0; i < entity_count; ++i) {
        NetId id{};
        std::uint8_t component_count = 0;
        if (!read_net_id(reader, id) || !reader.u8(component_count)) {
            ++malformed_;
            return;
        }
        // Resolve once per record. A miss means the reliable Spawn has not landed yet (see the
        // header) — we must still CONSUME this record's bytes, or the reader desynchronizes and
        // every later record in the packet is garbage.
        const ecs::Entity local = map_.resolve(id);
        bool resolved = local.is_valid();
        if (!resolved) {
            ++records_dropped_unmapped_;
        }

        for (std::uint8_t c = 0; c < component_count; ++c) {
            std::uint16_t wire_raw = 0;
            if (!reader.u16(wire_raw)) {
                ++malformed_;
                return;
            }
            ecs::ComponentId local_id{};
            const core::TypeInfo* type = nullptr;
            std::size_t packed = 0;
            if (!schema_.lookup(static_cast<WireComponentId>(wire_raw), local_id, type, packed)) {
                // An id outside the schema. Both peers derived the table from registries the
                // handshake proved identical, so this is a lying or corrupted peer rather than a
                // version skew — and we cannot know how many bytes to skip, so the rest of the
                // packet is unreadable. Abandon it.
                ++malformed_;
                return;
            }
            std::span<const std::byte> bytes;
            if (!reader.bytes(bytes, packed)) {
                ++malformed_;
                return;
            }
            if (!resolved) {
                continue; // bytes consumed, deliberately discarded
            }
            void* slot = world_->get_component_raw(local, local_id);
            if (slot == nullptr) {
                // The mirror does not carry this component yet — the server added it after the
                // spawn. add_component_raw relocates the entity between archetypes, so it must not
                // run while an archetype walk is in flight; here we are outside one (this is the
                // apply path, not the publish path), so it is safe.
                slot = world_->add_component_raw(local, local_id);
            }
            if (slot != nullptr && core::deserialize(*type, slot, bytes)) {
                // Stamp the write so the client's OWN change detection sees it — a client-side
                // consumer (interpolation at m11.6, the renderer) asks the same "changed since"
                // question the server does, and would silently miss replicated writes otherwise.
                world_->mark_changed_raw(local, local_id);
                ++deltas_applied_;
            }
        }
    }

    // Only after the whole packet parsed cleanly: a torn or malformed packet must not count toward
    // a tick's completeness, or the watermark could advance past a tick we never fully applied.
    acks_.observe(tick, part_index, part_count);
}

void ClientReplicator::send_ack(net::NetDriver& driver, std::uint64_t now_ms) {
    scratch_.clear();
    core::ByteWriter writer{scratch_};
    writer.u8(static_cast<std::uint8_t>(MessageTag::BaselineAck));
    writer.u64(acks_.watermark());
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session != nullptr && session->state() == net::SessionState::Connected) {
            (void)session->send_unreliable(scratch_, now_ms);
        }
    }
}

} // namespace rime::replication
