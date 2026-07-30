// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction_net/destruction_server.hpp"

#include <algorithm>

#include "rime/core/byte_cursor.hpp"
#include "rime/destruction/bind.hpp"

namespace rime::destruction_net {

namespace {

void write_op(core::ByteWriter& w, replication::NetId id, const destruction::DamageOp& op) {
    w.u32(id.index);
    w.u32(id.generation);
    w.u32(op.part);
    w.f32(op.amount);
    w.f32(op.impulse.x);
    w.f32(op.impulse.y);
    w.f32(op.impulse.z);
    w.f32(op.point.x);
    w.f32(op.point.y);
    w.f32(op.point.z);
    w.u8(op.central ? kFlagCentral : std::uint8_t{0});
}

} // namespace

void DestructionServer::publish(net::NetDriver& driver,
                                const replication::NetIdMap& map,
                                const ecs::World& world,
                                const destruction::DestructionWorld& destruction,
                                std::uint64_t tick,
                                std::uint64_t now_ms) {
    const std::span<const destruction::DamageOp> ops = destruction.committed_ops();
    if (ops.empty()) {
        return; // a quiet tick sends nothing — see the header on why not a keepalive
    }

    // Translate every op's local InstanceId to the NetId both peers share, ONCE, before any
    // packing. Doing it here rather than inside the per-client loop matters: the translation is
    // identical for every client, and at 64 players the difference is 64 walks of the instance
    // table per tick versus one.
    destruction::build_instance_entity_table(world, instance_to_entity_);

    std::vector<std::pair<replication::NetId, const destruction::DamageOp*>> addressed;
    addressed.reserve(ops.size());
    for (const destruction::DamageOp& op : ops) {
        const std::uint32_t idx = op.instance.index;
        if (idx >= instance_to_entity_.size() || !instance_to_entity_[idx].is_valid()) {
            ++unaddressable_; // damaged, but no entity is bound to it — nothing to name it by
            continue;
        }
        const std::optional<replication::NetId> net_id = map.net_id_of(instance_to_entity_[idx]);
        if (!net_id.has_value()) {
            ++unaddressable_; // bound to an entity the server never opted into replication
            continue;
        }
        addressed.emplace_back(*net_id, &op);
    }
    if (addressed.empty()) {
        return;
    }

    // How many packets this tick needs. Computed up front because part_count goes in EVERY part's
    // header — a receiver has to know a tick is incomplete from its first part, not discover it by
    // waiting for one that may never be described.
    const std::size_t total = addressed.size();
    const std::size_t parts = (total + kOpsPerPacket - 1) / kOpsPerPacket;
    const auto part_count =
        static_cast<std::uint8_t>(std::min<std::size_t>(parts, kMaxPartsPerTick));
    if (parts > 1) {
        ++multipart_ticks_;
    }

    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr || session->state() != net::SessionState::Connected) {
            continue;
        }
        for (std::uint8_t part = 0; part < part_count; ++part) {
            const std::size_t first = static_cast<std::size_t>(part) * kOpsPerPacket;
            const std::size_t last = std::min(first + kOpsPerPacket, total);

            scratch_.clear();
            core::ByteWriter w{scratch_};
            w.u8(static_cast<std::uint8_t>(MessageTag::DamageOps));
            w.u64(tick);
            w.u8(part);
            w.u8(part_count);
            w.u16(static_cast<std::uint16_t>(last - first));
            for (std::size_t i = first; i < last; ++i) {
                write_op(w, addressed[i].first, *addressed[i].second);
            }

            if (session->send_reliable(scratch_, now_ms)) {
                ++packets_sent_;
                ops_sent_ += last - first;
            }
            // A refused send is BACKPRESSURE (the 256-message backlog is full), not a lost packet:
            // the channel is reliable, so anything it accepted still arrives. Deliberately not
            // retried or queued here — a peer that cannot keep up with destruction is a peer the
            // game should decide about (session->disconnect()), and silently growing an unbounded
            // shadow queue behind a channel that already has one is how a memory leak gets built.
        }
    }
}

} // namespace rime::destruction_net
