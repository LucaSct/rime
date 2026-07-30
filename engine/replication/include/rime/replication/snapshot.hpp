// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>

#include "rime/ecs/chunk.hpp" // ecs::Version

// The replication wire format (m11.3) — the message tags and layouts that travel inside a
// net::Session's channel payloads, plus the marker component that opts an entity into replication.
//
// WHERE THESE BYTES SIT. A session datagram is [salt:4][channel header:13][payload]; everything
// below is the *payload*, so this tag namespace is entirely our own. It never collides with the
// session layer's control packets (those are discriminated at the datagram level by the high bit of
// the first byte, before the channel is even consulted — ADR-0033 A7).
//
// WHICH CHANNEL CARRIES WHAT, and why (ADR-0033 §2/§3):
//
//   Spawn / Despawn  → RELIABLE-ORDERED, server→client.  Structure is not superseded by newer
//                      structure: missing a despawn leaves a phantom entity forever, and no later
//                      message repairs it. These must arrive, exactly once, in order.
//   Delta            → UNRELIABLE-SEQUENCED, server→client.  State IS superseded by newer state, so
//                      a lost snapshot must be dropped rather than resent — resending it would
//                      deliver stale data behind fresher data, the one thing a real-time stream
//                      must never do.
//   BaselineAck      → UNRELIABLE-SEQUENCED, client→server.  Only the newest ack has any value, so
//                      it wants exactly the "supersede, never resend" contract. Putting it on the
//                      reliable channel would also let a per-tick ack compete with genuine events
//                      for the 32-packet in-flight window and the 256-message backlog — the least
//                      valuable message on the wire crowding out one of the most (ADR-0033 A9).
namespace rime::replication {

// ── THE SHARED PAYLOAD TAG SPACE ────────────────────────────────────────────────────────────────
//
// There is ONE session per peer pair, and every module that wants to say something sends it down
// that same session. The first payload byte is therefore a tag space SHARED between modules, not
// replication's private property — and until m11.4 nothing said so, because replication was the
// only tenant. It was a latent collision: the next module to define a message would naturally have
// started its own enum at 1, and a `DamageOps{1}` is indistinguishable on the wire from a
// `Spawn{1}`. Whichever module drained the session first would have consumed and misparsed the
// other's traffic, and `drain_received` MOVES messages out, so the loser would simply never see its
// own mail.
//
// The registry, allocated in blocks so a module can add messages without renegotiating:
//
//   0x01 – 0x3F   rime::replication   (m11.3 — this file)
//   0x40 – 0x7F   rime::destruction_net (m11.4 — the damage-op stream)
//   0x80 – 0xFF   unallocated
//
// A module MUST ignore tags outside its own block rather than treat them as errors, and MUST NOT
// consume them from a shared inbox — see ClientReplicator::apply_messages for the shape that makes
// both peers' subsystems able to read the same drained span.
inline constexpr std::uint8_t kTagBlockFirst = 0x01;
inline constexpr std::uint8_t kTagBlockLast = 0x3F;

[[nodiscard]] inline constexpr bool owns_tag(std::uint8_t tag) noexcept {
    return tag >= kTagBlockFirst && tag <= kTagBlockLast;
}

// Message discriminator, first byte of every replication payload.
enum class MessageTag : std::uint8_t {
    Spawn = 1,       // reliable:   server→client, a batch of NetIds to create mirrors for
    Despawn = 2,     // reliable:   server→client, a batch of NetIds whose mirrors must go
    Delta = 3,       // unreliable: server→client, component state changed since a baseline
    BaselineAck = 4, // unreliable: client→server, "the newest COMPLETE tick I hold"
};

// The marker that opts an entity into replication. Deliberately UNREFLECTED: it is a pure tag with
// no data, so there is nothing to serialize, and staying unreflected keeps it out of both the
// component schema hash and the wire schema — it is a local fact about an entity ("the server
// publishes this one" / "this is a mirror"), never a fact that travels.
struct Replicated {};

// The largest replication payload we will hand to a channel. ReliableChannel::kMaxPayload is 1200
// (sized to stay under the de-facto Internet MTU with no fragmentation anywhere); we keep a small
// reserve under it so a future framing tweak cannot silently start refusing sends.
inline constexpr std::size_t kMaxReplicationPayload = 1150;

// How many Delta packets one tick may split into. Bounded for two independent reasons: the client's
// completeness bitmask is 32 bits wide (see below), and an unbounded per-tick burst is exactly the
// unprioritized-broadcast failure that m11.5's relevancy and byte budgets exist to fix. Entities
// that do not fit are simply left for the next tick — the baseline mechanism re-offers them, so
// this degrades into latency rather than loss.
inline constexpr std::uint8_t kMaxDeltaPartsPerTick = 8;

// The stale-baseline valve. If a client's acknowledged baseline falls this far behind the world
// version, stop growing an ever-larger incremental delta (as the baseline recedes, "changed since"
// tends toward "everything", paid *every* tick the client stays behind) and re-seed it from
// scratch instead. Bounds worst-case per-tick server work to the cheap comparison pass, paying the
// expensive re-enumeration once per stall rather than once per tick of the stall.
inline constexpr ecs::Version kStaleBaselineTicks = 600;

// ── Wire layouts ────────────────────────────────────────────────────────────────────────────────
//
// All integers little-endian via core::ByteWriter/ByteReader — never a struct memcpy, so the bytes
// are identical on every compiler and CPU regardless of padding or host byte order.
//
//   Spawn        [tag:1][count:2] then count × [net_index:4][net_generation:4]
//   Despawn      [tag:1][count:2] then count × [net_index:4][net_generation:4]
//
//     Spawn carries NO component data — only identity. The state follows through the ordinary Delta
//     path, because a brand-new client's baseline is 0 and *everything* is "changed since 0". One
//     mechanism instead of two, and it keeps Spawn tiny: ~140 NetIds fit one datagram, so the
//     256-message reliable backlog covers tens of thousands of entities before pacing matters.
//
//   Delta        [tag:1][tick_version:8][part_index:1][part_count:1][entity_count:2]
//                then entity_count × [net_index:4][net_generation:4][component_count:1]
//                     then component_count × [wire_component_id:2][packed component bytes...]
//
//     No length prefix on the component bytes: the wire id names a type both peers agree on (the
//     schema hash proved it at handshake), and core::packed_size gives its exact framed length.
//     An entity's record is never split across parts — a torn tick then means "some entities are
//     fresher than others", which is what an unreliable snapshot stream already does under ordinary
//     loss, rather than a half-applied entity, which it never does.
//
//   BaselineAck  [tag:1][watermark:8]

// The client's acknowledgement state, and the reason it is not simply "the newest tick I saw".
//
// THE BUG THIS EXISTS TO AVOID. Suppose tick T splits into two parts. Part 2 arrives, part 1 is
// lost. If the client acked T on the strength of part 2 alone, the server would compute its next
// delta as "changed since T" — but the entities in the lost part 1 were written *at* T, not after
// it, so they would never be re-offered. If those entities then stop moving, they stay wrong on
// that client forever: a silent, permanent divergence, and precisely the property the milestone's
// convergence proof exists to catch.
//
// THE FIX. Advance the watermark only past a tick every part of which has been seen. Ticks arrive
// monotonically, so this needs no history ring — four values suffice, and a newer tick simply
// abandons an older incomplete one, which errs toward re-sending rather than toward divergence.
//
// (The alternative considered and rejected: let the client ack optimistically and have the server
// subtract a fixed safety margin from the acked baseline. That re-offers a write for `margin` ticks
// and is simpler, but it is *probabilistic* — lose the same part `margin` times running and the
// divergence returns. Under the deliberately-high loss the scripted-loss harness runs, that is the
// regime the proof actually exercises, so exactness is worth two bytes per packet.)
//
// A SECOND DOOR INTO THE SAME BUG, found by m11.4a (ADR-0033 A13). This class guards "did every
// part of the tick ARRIVE". It cannot see the other half: a packet that arrives whole and parses
// cleanly, but whose records cannot be APPLIED because their NetIds do not resolve yet — the
// deliberate cross-channel race of §3, where a reliable Spawn lands after the unreliable Delta that
// first names an entity. The bytes arrived; the state did not. A baseline is a promise about the
// second, so acknowledging on the strength of the first re-creates the permanent, silent divergence
// described above for any entity that stops changing before its Spawn lands — every static prop and
// every destructible wall standing quietly until someone shoots it.
//
// A13's answer was to withhold the acknowledgement so the server keeps re-offering. m11.4b replaced
// it with a better one (A14): HOLD the bytes and replay them the moment the Spawn binds the id.
// Nothing is lost, so the tick is honestly complete and can be acknowledged — which matters,
// because the A13 rule made acknowledgement hostage to spawn traffic and stalled the baseline for
// as long as entities kept arriving. Withholding survives only as the overflow fallback below.

// How many out-of-order component writes a client will hold while waiting for their Spawn. Sized
// for a spawn burst — a few hundred entities streaming in at once, each with a handful of
// components — and BOUNDED because the ids that key it are chosen by the peer, and an unbounded
// buffer a peer can grow is a denial of service wearing a resilience feature's clothes.
//
// Overflow is not silent: the oldest record is evicted, counted, and the tick carrying the eviction
// is left unacknowledged, which falls back to exactly the re-offer behaviour A13 shipped. So the
// buffer is an optimization over a correct-but-slower path, never load-bearing for correctness.
inline constexpr std::size_t kMaxDeferredRecords = 512;

class AckTracker {
public:
    // Record that part `part_index` of `tick` arrived, out of `part_count` total.
    void observe(ecs::Version tick, std::uint8_t part_index, std::uint8_t part_count) noexcept;

    // The newest tick every part of which has been seen — what BaselineAck reports.
    [[nodiscard]] ecs::Version watermark() const noexcept { return watermark_; }

private:
    [[nodiscard]] bool tracking_complete() const noexcept;

    ecs::Version tracking_tick_ = 0; // the tick whose parts we are currently accumulating
    std::uint32_t parts_seen_ = 0;   // bitmask over part indices of tracking_tick_
    std::uint8_t part_count_ = 0;    // how many parts tracking_tick_ was split into
    ecs::Version watermark_ = 0;     // newest COMPLETE tick
};

} // namespace rime::replication
