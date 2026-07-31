// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/client_replicator.hpp"

#include "rime/core/byte_cursor.hpp"
#include "rime/core/reflect/serialize.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/replication/interpolation.hpp"

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
    local_transform_id_ = world.component_id<ecs::LocalTransform>();
    // Client-only, and safe to be client-only: all three are unreflected, so they contribute to
    // neither the wire schema nor the component schema hash the handshake compares. WorldTransform
    // is registered here because a mirror needs one to be drawable at all (see
    // prepare_transform_write) and nothing else on a headless client would have registered it.
    previous_transform_id_ = world.register_component<PreviousTransform>();
    world_transform_id_ = world.register_component<ecs::WorldTransform>();
    render_transform_id_ = world.register_component<ecs::RenderTransform>();
}

void ClientReplicator::apply_inbound(net::NetDriver& driver) {
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session == nullptr) {
            continue;
        }
        inbox_.clear();
        (void)session->drain_received(inbox_);
        apply_messages(inbox_);
    }
}

void ClientReplicator::apply_messages(std::span<const net::Received> messages) {
    for (const net::Received& message : messages) {
        core::ByteReader reader{message.bytes};
        std::uint8_t tag = 0;
        if (!reader.u8(tag)) {
            ++malformed_;
            continue;
        }
        if (!owns_tag(tag)) {
            ++foreign_; // another module's block of the shared registry — not ours, not an error
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
                ++malformed_; // in OUR block but not a server→client message: BaselineAck travels
                              // the other way, and the rest of the block is unassigned
                break;
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
        // Anything that arrived for this id before it existed lands now, in arrival order.
        replay_deferred(id, local);
        ++spawns_applied_;
    }
}

void ClientReplicator::prepare_transform_write(ecs::Entity local,
                                               ecs::ComponentId component,
                                               const core::TypeInfo& type,
                                               std::span<const std::byte> incoming) {
    if (component != local_transform_id_) {
        return; // only transforms are placed and interpolated today
    }

    // Decode once: the same value answers both questions below.
    ecs::LocalTransform candidate{};
    const bool decoded = core::deserialize(type, &candidate, incoming);

    // (1) MAKE THE MIRROR DRAWABLE. propagate_transforms only touches entities that already have
    // BOTH LocalTransform and WorldTransform, and a mirror is spawned bare — so without this it
    // never acquires a world pose, and every renderer query (which reads WorldTransform) skips it
    // silently. A replicated entity was literally undrawable before m11.6b.
    //
    // Seeded from the incoming value rather than left default. A default would be identity for the
    // rest of this tick, and destruction::bind_destructibles reads WorldTransform in PREFERENCE to
    // LocalTransform (bind.cpp's placement_of) — so a bind running before this tick's
    // propagate_transforms would stand the instance at the world origin. The fallback that
    // comment describes stops covering us the moment the component exists, so it has to be right
    // on arrival, not one pass later.
    if (world_->get_component_raw(local, world_transform_id_) == nullptr) {
        void* slot = world_->add_component_raw(local, world_transform_id_);
        if (slot != nullptr && decoded) {
            static_cast<ecs::WorldTransform*>(slot)->value = candidate.value;
        }
    }

    // Fetched AFTER the add above: add_component_raw relocates the entity between archetypes, so a
    // pointer taken before it is dangling. Cheap to get wrong, silent when wrong.
    const void* existing = world_->get_component_raw(local, local_transform_id_);
    if (existing == nullptr) {
        return; // first write for this mirror — there IS no previous, and `valid` must stay false
    }

    // (2) ROLL THE HISTORY. An unchanged re-send is not a new tick's worth of motion. Skipping it
    // keeps `previous` at the last value that actually differed, which is what a renderer needs to
    // blend from.
    //
    // Compared FIELD BY FIELD, deliberately not with memcmp. `core::Quat` is over-aligned, so
    // `sizeof(Transform)` exceeds the 40 bytes it packs into and the remainder is padding whose
    // contents nothing defines — a memcmp reads it, never matches, and silently turns this guard
    // into a no-op. It did exactly that on the first attempt.
    if (decoded) {
        const auto& a = candidate.value;
        const auto& b = static_cast<const ecs::LocalTransform*>(existing)->value;
        const bool same = a.translation.x == b.translation.x &&
                          a.translation.y == b.translation.y &&
                          a.translation.z == b.translation.z && a.rotation.x == b.rotation.x &&
                          a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
                          a.rotation.w == b.rotation.w && a.scale.x == b.scale.x &&
                          a.scale.y == b.scale.y && a.scale.z == b.scale.z;
        if (same) {
            return;
        }
    }
    if (world_->get_component_raw(local, previous_transform_id_) == nullptr) {
        (void)world_->add_component_raw(local, previous_transform_id_);
        // Born together and destroyed together, so the render pass can select on the pair and never
        // meet a half-equipped mirror. Adding it lazily inside that pass is not an option: it walks
        // archetypes, and a structural change during a walk is exactly what query.hpp forbids.
        (void)world_->add_component_raw(local, render_transform_id_);
    }
    // Re-fetch BOTH after the add, for the same relocation reason as above.
    const auto* current = world_->get<ecs::LocalTransform>(local);
    auto* previous = world_->get<PreviousTransform>(local);
    if (current == nullptr || previous == nullptr) {
        return;
    }
    previous->value = current->value;
    previous->valid = true;
    previous->moved_this_tick = true; // consumed by settle_transform_history() at the tick's end
}

std::size_t ClientReplicator::settle_transform_history() {
    // Expire history that has stopped being renewed. `valid` means "a genuinely new value landed
    // during the tick that just ran", so it has to be turned off when a tick passes without one —
    // otherwise the last step replays every tick forever, because `alpha` sweeps 0→1 on its own
    // schedule and does not know this entity stopped moving.
    //
    // Keyed to a GENUINE change, not to "a record was applied". The server re-sends an unacked
    // value for a round trip, so records keep arriving for an entity that is standing still; a pass
    // that treated those as motion would hold the blend open for the whole re-send window and
    // produce exactly the sawtooth this exists to stop. The re-send guard in
    // prepare_transform_write is what tells the two apart, and this pass inherits that distinction
    // rather than re-deriving a weaker one.
    //
    // Expiring is safe the moment one full tick has passed: the blend it drove ran over that tick's
    // frames and reached alpha≈1, so the mirror is already drawn at `current` before this fires.
    std::size_t settled = 0;
    world_->query<PreviousTransform>().for_each([&](PreviousTransform& history) {
        if (history.moved_this_tick) {
            history.moved_this_tick = false; // one tick's worth of blend has now been on screen
            return;
        }
        if (history.valid) {
            history.valid = false;
            ++settled;
        }
    });
    histories_settled_ += settled;
    return settled;
}

void ClientReplicator::replay_deferred(NetId id, ecs::Entity local) {
    // Stable partition by hand rather than remove_if + a second pass: the held records for one id
    // must be applied in the order they arrived, because two writes to the same component are a
    // sequence and the last one is the current state.
    std::size_t write = 0;
    for (std::size_t read = 0; read < deferred_.size(); ++read) {
        DeferredRecord& record = deferred_[read];
        if (record.id.index != id.index || record.id.generation != id.generation) {
            if (write != read) {
                deferred_[write] = std::move(record);
            }
            ++write;
            continue;
        }
        const core::TypeInfo* type = nullptr;
        ecs::ComponentId unused{};
        std::size_t packed = 0;
        if (schema_.lookup(schema_.wire_id_of(record.component), unused, type, packed) &&
            type != nullptr) {
            prepare_transform_write(local, record.component, *type, record.bytes);
            void* slot = world_->get_component_raw(local, record.component);
            if (slot == nullptr) {
                slot = world_->add_component_raw(local, record.component);
            }
            if (slot != nullptr && core::deserialize(*type, slot, record.bytes)) {
                world_->mark_changed_raw(local, record.component);
                ++deltas_applied_;
                ++records_replayed_;
            }
        }
    }
    deferred_.resize(write);
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

    // Whether any record in this packet was parsed but THROWN AWAY because its NetId does not
    // resolve yet. See the acknowledgement rule at the bottom of this function.
    bool dropped_here = false;

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
        const bool resolved = local.is_valid();
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
                // HELD, not discarded (ADR-0033 A14). The Spawn is reliable and therefore certain
                // to arrive; keeping the bytes until it does means this tick lost nothing and can
                // be honestly acknowledged, instead of stalling the baseline for the whole spawn
                // burst.
                if (deferred_.size() >= kMaxDeferredRecords) {
                    deferred_.erase(deferred_.begin());
                    ++records_evicted_;
                    dropped_here = true; // fall back to the re-offer path for this tick only
                }
                DeferredRecord record;
                record.id = id;
                record.component = local_id;
                record.bytes.assign(bytes.begin(), bytes.end());
                deferred_.push_back(std::move(record));
                ++records_deferred_;
                continue;
            }
            prepare_transform_write(local, local_id, *type, bytes);
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

    // Only after the whole packet parsed cleanly AND every record in it actually landed.
    //
    // The parsed-cleanly half was always here: a torn packet must not count toward a tick's
    // completeness, or the watermark advances past a tick we never fully applied. The second half
    // closes a hole that m11.4's first end-to-end proof walked straight into, and it is worth
    // spelling out because the failure is invisible and permanent.
    //
    // A record whose NetId does not resolve is dropped — the ordinary cross-channel race, since the
    // reliable Spawn can land after the unreliable Delta that first mentions the entity. The packet
    // itself is intact, so the old code acked the tick. But "the packet arrived" and "the state in
    // it was applied" are different claims, and the baseline is a promise about the SECOND. The
    // server, believing the client holds tick T, computes every later delta as "changed since T" —
    // and the entity's write happened at or before T. If that entity then never changes again, it
    // is never re-offered: the mirror stays empty forever, silently.
    //
    // That "never changes again" is not a corner case. It is a static prop, a piece of level
    // geometry, a destructible wall standing quietly until someone shoots it — the very thing
    // m11.4 replicates. m11.3's own proof missed it because every entity there moved every tick, so
    // a dropped record was re-offered a tick later no matter what the ack said.
    //
    // Not acking is the conservative direction and needs no new machinery: the tick simply never
    // completes, the watermark stays below it, and the server keeps re-offering those writes until
    // a delta lands whose records ALL resolve — which is exactly the tick after the Spawn arrives.
    if (!dropped_here) {
        acks_.observe(tick, part_index, part_count);
    }
}

void ClientReplicator::send_ack(net::NetDriver& driver, std::uint64_t now_ms) {
    scratch_.clear();
    core::ByteWriter writer{scratch_};
    writer.u8(static_cast<std::uint8_t>(MessageTag::BaselineAck));
    writer.u64(acks_.watermark());
    for (const net::SessionId id : driver.session_ids()) {
        net::Session* session = driver.session(id);
        if (session != nullptr && session->state() == net::SessionState::Connected) {
            (void)session->send_unreliable(scratch_, now_ms, kStreamBaselineAck);
        }
    }
}

} // namespace rime::replication
