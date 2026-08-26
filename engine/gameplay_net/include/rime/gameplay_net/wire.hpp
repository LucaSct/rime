// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>

// The networked-player wire format (m12.3, ADR-0035 §3/§4).
//
// THIS MODULE ADDS EXACTLY ONE MESSAGE, and the interesting part of the design is everything it
// did NOT add. The pairing the whole brick exists to deliver — "this authoritative state is the
// result of your input up to sequence q" — crosses as an ORDINARY REPLICATED COMPONENT
// (`LastProcessedInput`, components.hpp) riding the snapshot path m11.3 already built. No new
// message kind, no new framing, no new completeness rule. ADR-0035 §4 argues why: a new message
// would need its own ordering story against the Delta that carries the state it describes, and two
// streams that must agree about a tick is exactly the class of bug m11.3's part/watermark
// machinery exists to prevent. Riding the same record makes them the same fact by construction.
//
// WHAT DOES NEED A MESSAGE: "which of these entities is ME." A client receives a world of NetIds
// and nothing in the snapshot distinguishes its own avatar from anyone else's — and the obvious
// shortcuts do not work. `LastProcessedInput` is on every player entity, and every client's
// sequence numbering starts at 1, so two players moving in step carry indistinguishable values.
// The session is the only thing that knows, and it is server-side only, so the server has to say
// it out loud.
namespace rime::gameplay_net {

// Message discriminator, first payload byte. These values live in the 0x80–0xBF block of the
// SHARED session tag registry documented in rime/replication/snapshot.hpp — one session carries
// every module's traffic, so the tag space is common property and a module that started its own
// enum at 1 would collide with replication's Spawn. Draining a session MOVES messages out, so that
// collision surfaces as one subsystem silently never receiving its mail; m11.4 learned it the
// expensive way and this block is claimed rather than assumed.
enum class MessageTag : std::uint8_t {
    AssignPlayer = 0x80, // reliable-ordered: server→client, "this NetId is your avatar"
};

inline constexpr std::uint8_t kTagBlockFirst = 0x80;
inline constexpr std::uint8_t kTagBlockLast = 0xBF;

[[nodiscard]] inline constexpr bool owns_tag(std::uint8_t tag) noexcept {
    return tag >= kTagBlockFirst && tag <= kTagBlockLast;
}

// ── Wire layout ─────────────────────────────────────────────────────────────────────────────────
//
// All integers little-endian through core::ByteWriter/ByteReader — never a struct memcpy, so the
// bytes are identical on every compiler and CPU whatever the padding or host byte order.
//
//   AssignPlayer  [tag:1][net_index:4][net_generation:4]
//
//     WHY RELIABLE-ORDERED. This is structure, not state: it is true once and forever, and no
//     later message repairs its loss. A client that missed it never knows which entity it is
//     driving — it would see its own avatar as just another remote player, and m12.4 would have
//     nothing to attach a prediction to. That is the Spawn/Despawn argument verbatim (ADR-0033 §3),
//     and it lands the message on the same channel for the same reason.
//
//     WHY IT CARRIES A NetId AND NOT AN ENTITY. An `ecs::Entity` is a slot in the SENDER's
//     directory and names nothing in the receiver's — the m11.3 rule that also keeps `ecs::Parent`
//     out of the wire schema. NetId is the only name both peers share.
//
//     WHY THE RECEIVER STORES THE NetId RATHER THAN RESOLVING IT ONCE. Reliable-ordered gives no
//     ordering against the UNRELIABLE Delta stream, and only a weak one against the reliable Spawn
//     that binds this id: the two are ordered only if the sender queued them in that order, which
//     is a property of the application's Publish stage rather than of the protocol. So a client can
//     legitimately be told "you are NetId{7,2}" a tick before it has any entity bound to {7,2}.
//     Resolving eagerly would produce a null entity and cache it forever; resolving through the
//     NetIdMap on every ask costs a vector index and cannot go stale. The generation check inside
//     `NetIdMap::resolve` then does the rest — if the server recycles index 7 for something else,
//     the stale assignment resolves to nothing rather than to a stranger's avatar.
//
//     A RE-SEND IS HARMLESS. The server re-announces on every reconnection and the receiver simply
//     overwrites; there is no "already assigned" error, because refusing one would turn a benign
//     retransmit into a broken session.
inline constexpr std::size_t kAssignPlayerBytes = 1 + 4 + 4;

} // namespace rime::gameplay_net
