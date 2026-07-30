// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction_net/destruction_client.hpp"

#include "rime/destruction/components.hpp"
#include "rime/destruction_net/composition.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/physics/world.hpp"

namespace rime::destruction_net {

namespace {

// The `ordinal`-th chunk of instance `instance` in this peer's roster, or kUnboundDebris. Linear
// over the roster, which is the honest cost of not caching a mapping that would mislabel rubble if
// it ever drifted from the roster it describes; the roster is small and this runs only until a
// mirror binds.
[[nodiscard]] std::uint32_t find_debris(const destruction::DestructionWorld& destruction,
                                        std::uint32_t instance,
                                        std::uint32_t ordinal) noexcept {
    std::uint32_t seen = 0;
    for (std::size_t d = 0; d < destruction.debris_count(); ++d) {
        if (destruction.debris_source(d).index != instance) {
            continue;
        }
        if (seen == ordinal) {
            return static_cast<std::uint32_t>(d);
        }
        ++seen;
    }
    return kUnboundDebris;
}

} // namespace

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
        switch (static_cast<MessageTag>(tag)) {
            case MessageTag::DamageOps:
                on_damage_ops(reader, map, world);
                break;
            case MessageTag::CompositionCheck:
                on_composition_check(reader);
                break;
            default:
                ++malformed_; // our block, but not a message we define
                break;
        }
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

void DestructionClient::on_composition_check(core::ByteReader& reader) {
    std::uint64_t tick = 0;
    std::uint16_t count = 0;
    if (!reader.u64(tick) || !reader.u16(count)) {
        ++malformed_;
        return;
    }
    // Attach to the batch it describes rather than to the client. The channel is ordered, so that
    // batch is already queued; if it is not, the check names a tick we never assembled and there is
    // nothing to compare it against.
    Batch* batch = nullptr;
    for (Batch& b : ready_) {
        if (b.tick == tick) {
            batch = &b;
            break;
        }
    }
    if (batch == nullptr) {
        // The batch this describes has already been applied, so the state it attests to is gone —
        // later batches have been applied on top of it. Counted rather than ignored: this is the
        // one path in this module that could silently reduce how much the composition check
        // actually checks, and a proof asserting "matches > 0" would still pass while verifying
        // almost nothing.
        ++composition_unverified_;
        return;
    }
    batch->expected.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        ExpectedComposition e;
        if (!reader.u32(e.source.index) || !reader.u32(e.source.generation) ||
            !reader.u64(e.hash)) {
            ++malformed_;
            batch->expected.clear();
            return;
        }
        batch->expected.push_back(e);
    }
}

void DestructionClient::verify_composition(const ecs::World& world,
                                           const replication::NetIdMap& map,
                                           const destruction::DestructionWorld& destruction) {
    for (const ExpectedComposition& e : pending_verify_) {
        const ecs::Entity entity = map.resolve(e.source);
        if (!entity.is_valid()) {
            // The wall's own mirror has not landed, so there is nothing to compare against — and by
            // the time it does, this batch's state will have been built over. Counted, for the same
            // reason the orphan case is: an uncounted skip makes the check quietly check less.
            ++composition_unverified_;
            continue;
        }
        const auto* ref = world.get<destruction::DestructibleInstanceRef>(entity);
        if (ref == nullptr || ref->instance == destruction::kUnboundInstance) {
            ++composition_unverified_; // arrived but not standing yet — same story
            continue;
        }
        const std::uint64_t local =
            debris_composition_hash(destruction, destruction::InstanceId{ref->instance, 0});
        if (local != e.hash) {
            ++composition_mismatches_;
        } else {
            ++composition_matches_;
        }
    }
    pending_verify_.clear();
}

void DestructionClient::flush() {
    if (pending_count_ > 1) {
        ++multipart_ticks_;
    }
    // The tick is complete but NOT applied: it goes to the back of the queue and waits for a
    // fracture boundary of its own (see the header on why merging two ticks diverges the debris
    // roster even when every alive bit still agrees).
    ready_.push_back(Batch{pending_tick_, std::move(pending_), {}});
    pending_.clear();
    pending_seen_ = 0;
    pending_count_ = 0;
}

bool DestructionClient::apply_next_batch(destruction::DestructionWorld& destruction) {
    if (ready_.empty()) {
        return false;
    }
    Batch& batch = ready_.front();
    // A batch released while the PREVIOUS one's fingerprints are still awaiting comparison. That
    // happens whenever a client catches up by pumping several batches in a tick: verification runs
    // once, at sync_debris, so only the last batch's expectations can still be checked against a
    // state that matches them. The earlier ones describe states already built over.
    //
    // Counted rather than dropped. Verifying each batch at its own fracture boundary would need the
    // verification to move inside the catch-up step (it needs the world and the NetIdMap, which
    // this call does not take) — a real improvement, and a named follow-up rather than something to
    // half-do here. Until then this is visible instead of invisible.
    composition_unverified_ += pending_verify_.size();

    destruction.apply_remote_ops(batch.ops);
    ops_applied_ += batch.ops.size();
    ++ticks_applied_;
    pending_verify_ = std::move(batch.expected);
    ready_.pop_front();
    return true;
}

void DestructionClient::sync_debris(const ecs::World& world,
                                    const replication::NetIdMap& map,
                                    destruction::DestructionWorld& destruction,
                                    physics::PhysicsWorld& physics,
                                    float tolerance_m) {
    // const_cast for the same reason bind.cpp does it: query() hands out mutable component
    // references and so cannot be const, but this pass only reads the world. It DOES write
    // DebrisRef — the client's private roster link — which is why the ref is collected here and
    // written in the second pass rather than mutated mid-iteration.
    ecs::World& mutable_world = const_cast<ecs::World&>(world);

    // The composition check for the batch just applied (m11.4b). Done here rather than at decode
    // time because "the same shape" is only a meaningful question after the fracture boundary that
    // batch owns has actually run.
    verify_composition(world, map, destruction);

    const float tolerance_sq = tolerance_m * tolerance_m;

    mutable_world.query<DebrisOrigin, ecs::LocalTransform>().for_each(
        [&](ecs::Entity e, DebrisOrigin& origin, ecs::LocalTransform& transform) {
            // Resolve the mirror to one of OUR chunks, once, and remember it. The ordinal is a
            // position in the source instance's own debris sequence, which both peers derive
            // identically (m11.4a A12) — so this is a lookup, never a guess.
            DebrisRef* ref = mutable_world.get<DebrisRef>(e);
            if (ref == nullptr || ref->debris == kUnboundDebris) {
                const replication::NetId source_id{origin.source_net_index,
                                                   origin.source_net_generation};
                const ecs::Entity source_entity = map.resolve(source_id);
                if (!source_entity.is_valid()) {
                    ++debris_unresolved_;
                    return; // the wall this fell off has not arrived yet
                }
                const auto* instance_ref =
                    world.get<destruction::DestructibleInstanceRef>(source_entity);
                if (instance_ref == nullptr ||
                    instance_ref->instance == destruction::kUnboundInstance) {
                    ++debris_unresolved_;
                    return; // arrived but not standing yet
                }
                const std::uint32_t local_debris =
                    find_debris(destruction, instance_ref->instance, origin.ordinal);
                if (local_debris == kUnboundDebris) {
                    ++debris_unresolved_; // our own fracture has not produced this chunk yet
                    return;
                }
                if (ref != nullptr) {
                    ref->debris = local_debris;
                } else {
                    ref = mutable_world.add_component(e, DebrisRef{local_debris});
                    if (ref == nullptr) {
                        return;
                    }
                }
                ++debris_bound_;
            }

            const physics::BodyId body = destruction.debris_body(ref->debris);
            physics::BodyState local{};
            if (!physics.get_body_state(body, local)) {
                return; // frozen or gone: set_debris_state would refuse anyway
            }

            // Correct only on real disagreement — see the header on why not every tick.
            const core::Vec3 delta = transform.value.translation - local.position;
            if (core::length_squared(delta) <= tolerance_sq) {
                return;
            }
            physics::BodyState corrected = local;
            corrected.position = transform.value.translation;
            corrected.orientation = transform.value.rotation;
            // Velocity is deliberately KEPT from the local solver. It is not replicated in v1 (the
            // ROADMAP promises transforms), and the local value is a good estimate precisely
            // because both peers started this chunk from the same impulse. Overwriting it with zero
            // — the obvious alternative — would make every correction visibly stall the rubble.
            destruction.set_debris_state(ref->debris, corrected, physics);
            ++debris_corrections_;
        });
}

} // namespace rime::destruction_net
