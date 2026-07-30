// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>

// The networked-destruction wire format (m11.4, ADR-0033 A1 + A11).
//
// WHAT CROSSES, AND WHY IT IS THE OP LIST. ADR-0033 §2 originally said clients "replay the
// deterministic damage → detach function", and amendment A1 sharpened that: replaying *commands*
// does not work, because half the damage stream is emergent rather than commanded — the runtime
// converts the local solver's contact impulses into damage ops every tick. So the replicated
// artifact is the committed, canonically-ordered DAMAGE-OP LIST, which was always the deterministic
// function's input. The server runs the full contact→damage conversion; clients apply what they are
// sent and never convert their own (destruction::Authority::Remote is that suppression).
//
// WHICH CHANNEL, AND WHY IT IS THE RELIABLE ONE. Destruction is not state that newer state
// supersedes — it is a sequence of transitions, each of which permanently changes what the next one
// means. Lose the op that killed the part holding up an arch and no later message repairs it: the
// arch stands on this client forever, and every subsequent op lands on a wall of a different shape.
// That is the exact opposite of a transform snapshot, and it is why the split in ADR-0033 §3
// exists. Damage ops therefore ride RELIABLE-ORDERED, server→client. m11.4b's debris transforms
// will ride the unreliable channel, for precisely the reason ops do not.
//
// WHAT "APPLY AT THE SAME TICK" MEANS (amendment A11). The batch is tick-tagged, but the tag is NOT
// an instruction to stall until the client's local tick counter reads T — a client is not running
// lockstep, and reliable delivery is ordered without being timely, so a batch for T routinely lands
// when the local sim is at T+3. The tag is an ordering and identity key: it names the batch, it is
// what m11.4b associates debris with, and it is what m11.6's interpolation will use to schedule the
// visible break. Clients apply each batch AS IT ARRIVES, in the authority's order, against the same
// prior state — the sameness that matters is position in the op sequence, not agreement about a
// clock. The break shows a few ticks late; because debris transforms are replicated, that offset is
// a fixed presentation delay rather than an error that accumulates.
namespace rime::destruction_net {

// Message discriminator, first payload byte. These values live in the 0x40–0x7F block of the SHARED
// session tag registry documented in rime/replication/snapshot.hpp — one session carries every
// module's traffic, so the tag space is common property. Starting a fresh enum at 1 here would have
// collided with replication's Spawn, and since draining a session MOVES messages out, the collision
// would have shown up as one subsystem silently never receiving its mail.
enum class MessageTag : std::uint8_t {
    DamageOps = 0x40,        // reliable-ordered: server→client, one tick's committed damage-op list
    CompositionCheck = 0x41, // reliable-ordered: server→client, the post-batch debris fingerprint
};

inline constexpr std::uint8_t kTagBlockFirst = 0x40;
inline constexpr std::uint8_t kTagBlockLast = 0x7F;

[[nodiscard]] inline constexpr bool owns_tag(std::uint8_t tag) noexcept {
    return tag >= kTagBlockFirst && tag <= kTagBlockLast;
}

// ── Wire layout ─────────────────────────────────────────────────────────────────────────────────
//
// All integers little-endian through core::ByteWriter/ByteReader — never a struct memcpy, so the
// bytes are identical on every compiler and CPU whatever the padding or host byte order.
//
//   DamageOps  [tag:1][tick:8][part_index:1][part_count:1][op_count:2]
//              then op_count × [net_index:4][net_generation:4][part:4]
//                              [amount:4][impulse:12][point:12][flags:1]
//
//     The instance is named by the NetId of the destructible's ENTITY, never by an InstanceId: that
//     is a local table position, and two peers agree on it only if they happened to spawn in the
//     same order — which late-join breaks on its first tick, permanently and silently. The receiver
//     translates NetId → entity (m11.3's NetIdMap) → its own InstanceId (the bind table). Neither
//     side's index ever crosses.
//
//     `flags` bit 0 is the op's `central` bit: whether the impulse pushes through the COM (an
//     explicit blast) or at `point` (a contact, where the lever arm is part of the look).
//
//     Ops are sent EXPANDED per-part with the radius falloff already resolved, not as the
//     apply_damage calls that produced them. That arithmetic then happens on exactly one machine
//     and cannot be disagreed about; replicating the call instead would re-run a distance query and
//     a falloff multiply on every client, on a different compiler, which is how sub-ULP drift gets
//     in.
//   CompositionCheck  [tag:1][tick:8][count:2]
//                     then count × [net_index:4][net_generation:4][composition_hash:8]
//
//     Sent immediately AFTER the DamageOps parts for the same tick, on the same reliable-ordered
//     channel — so ordered delivery alone guarantees it arrives once every op of that tick has,
//     with no completeness rule of its own to get wrong. One entry per destructible the batch
//     touched, carrying that instance's debris composition as the authority left it
//     (composition.hpp).
//
//     A separate message rather than a trailer on DamageOps, deliberately. A trailer would have to
//     live in exactly one part of a multi-part tick, which means the packer computing part sizes
//     around a payload that is not ops — the kind of coupling that is fine until the day a tick
//     splits differently than expected. Ordering already gives us everything the trailer would.
inline constexpr std::uint8_t kFlagCentral = 0x01;

// Fixed sizes, so the packer can compute how many ops fit without trial serialization.
inline constexpr std::size_t kDamageOpsHeaderBytes = 13; // tag + tick + part idx/count + op count
inline constexpr std::size_t kDamageOpBytes = 41; // net id 8 + part 4 + amount 4 + vec3 ×2 + flags

// The largest payload we hand to a channel. ReliableChannel::kMaxPayload is 1200, sized to stay
// under the de-facto Internet MTU with no fragmentation anywhere; the reserve under it matches
// replication's, so a future framing tweak cannot silently start refusing sends.
inline constexpr std::size_t kMaxPayload = 1150;

// How many ops fit in one packet — 27 at the sizes above.
inline constexpr std::size_t kOpsPerPacket = (kMaxPayload - kDamageOpsHeaderBytes) / kDamageOpBytes;

// A tick's op list may split across this many packets. Unlike m11.3's Delta parts this is NOT a
// completeness-bitmask limit (the reliable channel loses nothing, so there is no bitmask); it is a
// sanity bound on one tick's destruction, and the point at which a receiver should conclude the
// sender is lying rather than that a wall was very large. 27 × 64 ≈ 1700 ops in a tick, which is a
// whole building's collapse.
inline constexpr std::uint8_t kMaxPartsPerTick = 64;

} // namespace rime::destruction_net
