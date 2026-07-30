// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction_net/destruction_client.hpp"

#include "rime/destruction/components.hpp"

namespace rime::destruction_net {

void DestructionClient::apply_inbound(net::NetDriver& driver,
                                      const replication::NetIdMap& map,
                                      const ecs::World& world) {
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr) {
            continue;
        }
        inbox_.clear();
        (void)session->drain_received(inbox_);
        apply_messages(inbox_, map, world);
    }
}

void DestructionClient::apply_messages(std::span<const net::Received> messages,
                                       const replication::NetIdMap& map,
                                       const ecs::World& world) {
    for (const net::Received& message : messages) {
        core::ByteReader reader{message.bytes};
        std::uint8_t tag = 0;
        if (!reader.u8(tag)) {
            ++malformed_;
            continue;
        }
        if (!owns_tag(tag)) {
            continue; // replication's, or a module that does not exist yet — leave it in the span
        }
        if (static_cast<MessageTag>(tag) != MessageTag::DamageOps) {
            ++malformed_; // our block, but not a message we define
            continue;
        }
        on_damage_ops(reader, map, world);
    }
}

void DestructionClient::on_damage_ops(core::ByteReader& reader,
                                      const replication::NetIdMap& map,
                                      const ecs::World& world) {
    std::uint64_t tick = 0;
    std::uint8_t part_index = 0;
    std::uint8_t part_count = 0;
    std::uint16_t op_count = 0;
    if (!reader.u64(tick) || !reader.u8(part_index) || !reader.u8(part_count) ||
        !reader.u16(op_count)) {
        ++malformed_;
        return;
    }
    if (part_count == 0 || part_count > kMaxPartsPerTick || part_index >= part_count) {
        ++malformed_; // a self-inconsistent header: the sender is lying, not merely out of date
        return;
    }

    // A part of a DIFFERENT tick than the one we are accumulating means the previous tick never
    // completed. On a reliable-ordered channel that cannot happen from loss — so it is either the
    // first part we have ever seen, or a sender that mis-split a batch. Abandoning the partial is
    // the safe response: applying half a tick's canonical sequence is precisely the divergence the
    // atomicity rule exists to prevent (see the header).
    //
    // `pending_count_ != 0` is the mid-accumulation flag, deliberately not `!pending_.empty()`: a
    // tick every one of whose ops was dropped as unmapped is still a tick being accumulated, and
    // testing the op vector would silently restart accumulation on its next part.
    if (pending_count_ != 0 && tick != pending_tick_) {
        ++malformed_;
        pending_.clear();
        pending_seen_ = 0;
        pending_count_ = 0;
    }
    if (pending_count_ == 0) {
        pending_tick_ = tick;
        pending_count_ = part_count;
        pending_seen_ = 0;
        pending_.clear();
    }

    for (std::uint16_t i = 0; i < op_count; ++i) {
        replication::NetId net_id{};
        destruction::DamageOp op;
        std::uint8_t flags = 0;
        if (!reader.u32(net_id.index) || !reader.u32(net_id.generation) || !reader.u32(op.part) ||
            !reader.f32(op.amount) || !reader.f32(op.impulse.x) || !reader.f32(op.impulse.y) ||
            !reader.f32(op.impulse.z) || !reader.f32(op.point.x) || !reader.f32(op.point.y) ||
            !reader.f32(op.point.z) || !reader.u8(flags)) {
            ++malformed_;
            return; // truncated mid-record: the rest of this packet is not trustworthy either
        }
        op.central = (flags & kFlagCentral) != 0;

        // NetId → entity → local InstanceId. Either hop can legitimately miss: the entity's Spawn
        // is reliable but its LocalTransform rides the unreliable delta path, so a mirror can exist
        // before it can be BOUND, and an op for it arrives with nowhere to go. That is the ordinary
        // cross-channel race ADR-0033 §3 creates on purpose, not an error — the op is dropped, and
        // the state-application seam (A3) is what repairs a mirror that missed real destruction.
        const ecs::Entity entity = map.resolve(net_id);
        if (!entity.is_valid()) {
            ++dropped_unmapped_;
            continue;
        }
        const destruction::DestructibleInstanceRef* ref =
            world.get<destruction::DestructibleInstanceRef>(entity);
        if (ref == nullptr || ref->instance == destruction::kUnboundInstance) {
            ++dropped_unmapped_;
            continue;
        }
        op.instance = destruction::InstanceId{ref->instance, 0};
        pending_.push_back(op);
    }

    // Mark this part seen, and flush once every part of the tick has been. A bitmask rather than a
    // running count so a duplicated part cannot complete a tick that is still missing one — the
    // reliable channel promises exactly-once, but a completeness test that silently depends on that
    // promise is a test worth not writing.
    pending_seen_ |= std::uint64_t{1} << part_index;

    const std::uint64_t complete = pending_count_ >= 64
                                       ? ~std::uint64_t{0}
                                       : (std::uint64_t{1} << pending_count_) - std::uint64_t{1};
    if (pending_seen_ == complete) {
        flush();
    }
}

void DestructionClient::flush() {
    if (pending_count_ > 1) {
        ++multipart_ticks_;
    }
    // The tick is complete but NOT applied: it goes to the back of the queue and waits for a
    // fracture boundary of its own (see the header on why merging two ticks diverges the debris
    // roster even when every alive bit still agrees).
    ready_.push_back(Batch{pending_tick_, std::move(pending_)});
    pending_.clear();
    pending_seen_ = 0;
    pending_count_ = 0;
}

bool DestructionClient::apply_next_batch(destruction::DestructionWorld& destruction) {
    if (ready_.empty()) {
        return false;
    }
    Batch& batch = ready_.front();
    destruction.apply_remote_ops(batch.ops);
    ops_applied_ += batch.ops.size();
    ++ticks_applied_;
    ready_.pop_front();
    return true;
}

} // namespace rime::destruction_net
