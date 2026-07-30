// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction_net/destruction_server.hpp"

#include <algorithm>

#include "rime/core/byte_cursor.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction_net/composition.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/physics/world.hpp"

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

    // One composition fingerprint per destructible this batch touched, computed once for every
    // client. Deduplicated by NetId: a wall taking twenty ops in a tick is still one instance whose
    // rubble has one shape.
    std::vector<std::pair<replication::NetId, std::uint64_t>> checked;
    for (const auto& entry : addressed) {
        const replication::NetId net_id = entry.first;
        const bool seen = std::any_of(checked.begin(), checked.end(), [net_id](const auto& e) {
            return e.first.index == net_id.index && e.first.generation == net_id.generation;
        });
        if (!seen) {
            checked.emplace_back(net_id,
                                 debris_composition_hash(destruction, entry.second->instance));
        }
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

        // The composition check for this tick, after its ops (m11.4b). Ordered delivery is what
        // makes "after" mean anything: the client can compare the instant it lands, because every
        // op it describes has already been applied.
        if (!checked.empty()) {
            scratch_.clear();
            core::ByteWriter w{scratch_};
            w.u8(static_cast<std::uint8_t>(MessageTag::CompositionCheck));
            w.u64(tick);
            w.u16(static_cast<std::uint16_t>(checked.size()));
            for (const auto& entry : checked) {
                w.u32(entry.first.index);
                w.u32(entry.first.generation);
                w.u64(entry.second);
            }
            if (session->send_reliable(scratch_, now_ms)) {
                ++composition_checks_sent_;
            }
        }
    }
}

void DestructionServer::sync_debris(ecs::World& world,
                                    const destruction::DestructionWorld& destruction,
                                    const physics::PhysicsWorld& physics,
                                    const replication::NetIdMap& map,
                                    const std::function<void(ecs::Entity)>& replicate,
                                    const std::function<void(ecs::Entity)>& despawn) {
    const std::size_t roster = destruction.debris_count();
    debris_to_entity_.resize(roster, ecs::kNullEntity);

    // The ordinal of each chunk among its OWN instance's debris, in roster order. Recomputed each
    // tick rather than cached, because it is the receiver's only handle on which chunk this is and
    // a cache that drifted from the roster would mislabel rubble rather than fail loudly. One pass
    // over an append-only roster; the counters are a small map keyed by instance index.
    destruction::build_instance_entity_table(world, instance_to_entity_);
    std::vector<std::uint32_t> next_ordinal(instance_to_entity_.size(), 0);

    for (std::size_t d = 0; d < roster; ++d) {
        const destruction::InstanceId source = destruction.debris_source(d);
        const std::uint32_t src = source.index;
        if (src >= next_ordinal.size()) {
            continue; // a chunk whose source instance is not in the bind table at all
        }
        const std::uint32_t ordinal = next_ordinal[src]++;

        const physics::BodyId body = destruction.debris_body(d);
        const bool live = physics.is_alive(body);

        // Reclaimed by the M8.5 lifecycle: retract the mirror rather than leaving a phantom chunk
        // frozen mid-air on every client with nothing that ever repairs it.
        if (!live) {
            if (debris_to_entity_[d].is_valid()) {
                despawn(debris_to_entity_[d]);
                debris_to_entity_[d] = ecs::kNullEntity;
                ++debris_retracted_;
            }
            continue;
        }

        physics::BodyState state{};
        if (!physics.get_body_state(body, state)) {
            continue;
        }
        core::Transform placement;
        placement.translation = state.position;
        placement.rotation = state.orientation;

        if (!debris_to_entity_[d].is_valid()) {
            // A chunk the receiver cannot be told the origin of is not replicated at all. Sending
            // it under an unresolvable name would put rubble on the client that nothing ever moves
            // or removes — worse than the chunk the client derives for itself and simulates
            // locally.
            const ecs::Entity source_entity = instance_to_entity_[src];
            if (!source_entity.is_valid()) {
                ++debris_unaddressable_;
                continue;
            }
            const std::optional<replication::NetId> source_id = map.net_id_of(source_entity);
            if (!source_id.has_value()) {
                ++debris_unaddressable_;
                continue;
            }
            const ecs::Entity e =
                world.spawn_with(ecs::LocalTransform{placement},
                                 DebrisOrigin{source_id->index, source_id->generation, ordinal});
            debris_to_entity_[d] = e;
            replicate(e);
            ++debris_spawned_;
            continue; // the transform it was just spawned with IS this tick's
        }

        // Refresh the transform and stamp it changed, so the server's own change detection — the
        // per-column chunk versions m11.3's delta pass compares — actually sees the write. A silent
        // in-place edit would be replicated once at spawn and then never again, which is the same
        // class of bug as A13 and just as quiet.
        const ecs::Entity e = debris_to_entity_[d];
        if (auto* transform = world.get<ecs::LocalTransform>(e)) {
            transform->value = placement;
            world.mark_changed<ecs::LocalTransform>(e);
        }
    }
}

} // namespace rime::destruction_net
