// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Session control packets (m11.2, ADR-0033 §7) — the connectionless half of the session wire
// protocol, as a pure codec: no NetDriver, no Link, no clock, so the whole thing is unit-testable
// by feeding it byte spans.
//
// Why connectionless: a handshake cannot ride ReliableChannel. A channel is a conversation with an
// ESTABLISHED peer — sequence spaces, ack state, resend queues — none of which exists until both
// sides agree there is a conversation. Worse, allocating a channel on first sight of an endpoint
// lets every spoofed datagram conjure heavyweight state (the classic unbounded-map DoS). So
// handshake, heartbeat, and disconnect travel as small standalone datagrams with their own
// retry/timeout (owned by Session/NetDriver), and a channel is allocated only on acceptance.
//
// Wire framing: every datagram the session layer emits — control OR channel — begins with a 4-byte
// salt (net_driver.hpp argues why; the short version is incarnation safety). The layouts below are
// what follows that salt. The first payload byte discriminates: channel packets keep the m11.1
// ReliableChannel enum (values 0..2), control tags all carry the 0x80 high bit, so the split costs
// one comparison and the channel path stays byte-identical to m11.1.
//
// All fields are little-endian via core::ByteWriter/ByteReader — never memcpy of structs
// (platform/socket.hpp's host-byte-order lesson: nothing host-layout crosses the wire). Decoders
// require EXACT consumption: trailing bytes mean corruption, because protocol_version — not
// trailing garbage — is the extension mechanism.
//
// Layouts (sizes in bytes, after the [salt:4] frame):
//
//   ConnectRequest (21): [tag:1][magic:2][protocol:2][app_id:4][schema_hash:8][client_salt:4]
//     The frame salt IS client_salt — there is no shared incarnation yet (Session's adapter makes
//     this fall out naturally: while Connecting, the session's salt is its client_salt). The
//     identity triple travels as SEPARATE fields, compared separately, so a rejection names the
//     exact thing to fix — ADR-0033 §4's actionable-rejection promise.
//
//   ConnectAccept (11):  [tag:1][magic:2][client_salt:4][server_salt:4]
//     Echoes client_salt so the client matches the reply to its outstanding attempt. The
//     incarnation salt both sides use from now on is fold(client_salt, server_salt). The frame
//     salt of this datagram is client_salt — the only value the client recognizes yet.
//
//   ConnectReject (24):  [tag:1][magic:2][client_salt:4][reason:1][expected:8][actual:8]
//     Carries the mismatching numbers (u16/u32 fields widened to u64) so the client can print
//     "server runs schema 0x…, you run 0x…". A rejection a human cannot act on is a bug report,
//     not a diagnostic.
//
//   Disconnect (4):      [tag:1][magic:2][reason:1]
//     Meaningful only with the incarnation salt in its frame — a stale incarnation's Disconnect
//     must not murder the fresh session at the same endpoint.
//
//   Heartbeat (3):       [tag:1][magic:2]
//     Payload-free liveness for an idle connection; likewise salt-guarded.
namespace rime::net {

// Frame size every session-layer datagram pays before the payload above.
inline constexpr std::size_t kSaltFrameSize = 4;

// Bumped BY HAND whenever any wire layout in this file changes. Two processes of the same build
// always agree; this guards stale binaries meeting on a LAN.
inline constexpr std::uint16_t kProtocolVersion = 1;

// Guards the control space against random garbage: a datagram that fails the magic check is a
// clean silent drop plus a counter — never an allocation, never a log line (a packet flood must
// not become a log flood).
inline constexpr std::uint16_t kControlMagic = 0x5253; // "RS"

// Salt value reserved to mean "no session". The driver's fold() and PRNG never produce it.
inline constexpr std::uint32_t kNoSalt = 0;

enum class ControlTag : std::uint8_t {
    ConnectRequest = 0x81, // client -> server: "may I join, and here is who I am"
    ConnectAccept = 0x82,  // server -> client: "yes; here is our incarnation"
    ConnectReject = 0x83,  // server -> client: "no, and here is exactly why"
    Disconnect = 0x84,     // either way: "this incarnation is over"
    Heartbeat = 0x85,      // either way, idle only: "still here"
};

inline constexpr std::uint8_t kControlTagMask = 0x80;

// The one-byte demultiplex the driver performs before anything else. ReliableChannel::
// process_packet interprets byte 0 as ITS channel enum and runs process_ack() before switching on
// it, so a control packet must never reach it — the driver reads this bit first and keeps the two
// worlds apart.
[[nodiscard]] constexpr bool is_control_tag(std::byte first_payload_byte) noexcept {
    return (static_cast<std::uint8_t>(first_payload_byte) & kControlTagMask) != 0;
}

// Why a session ended or failed to start — one enum for both, because the game handles them in the
// same place (the drained SessionEvent stream). The last four values double as the ConnectReject
// wire reasons. Lives here (not session.hpp) because the reject codec needs it and this header
// must stay dependency-free. to_string() exists so a rejection reaches a HUMAN with an actionable
// message.
enum class DisconnectReason : std::uint8_t {
    Graceful,         // the peer asked to leave
    LocalClose,       // we asked to leave
    Timeout,          // silence past timeout_ms — peer death, the m11.2 milestone proof
    Replaced,         // our peer reconnected as a new incarnation; this session was superseded
    ConnectTimeout,   // no answer to ConnectRequest after all retries
    ProtocolMismatch, // kProtocolVersion differs — a stale binary is on the wire
    AppMismatch,      // app_id differs — two different games found each other on a LAN
    SchemaMismatch,   // reflected-component builds differ — rebuild/re-cook the client
    ServerFull,       // the server's session table is at max_sessions
};

[[nodiscard]] std::string_view to_string(DisconnectReason reason) noexcept;

// Decoded payloads. client_salt is echoed in Accept/Reject so the client can match a reply to its
// outstanding attempt — a reply to somebody else's attempt is a clean drop.
struct ConnectRequest {
    std::uint16_t protocol = 0;
    std::uint32_t app_id = 0;
    std::uint64_t schema_hash = 0;
    std::uint32_t client_salt = 0;
};

struct ConnectAccept {
    std::uint32_t client_salt = 0;
    std::uint32_t server_salt = 0;
};

struct ConnectReject {
    std::uint32_t client_salt = 0;
    DisconnectReason reason = DisconnectReason::Graceful;
    std::uint64_t expected = 0; // what the server runs
    std::uint64_t actual = 0;   // what the requester sent
};

// Encode/decode pairs, one per packet type. Encoders return the PAYLOAD (the caller adds the salt
// frame). Decoders take the payload after the salt and return nullopt on any deviation — wrong
// tag, wrong magic, short buffer, trailing bytes, out-of-range reason. Untrusted-input discipline:
// everything goes through core::ByteReader's checked reads; there is no hand-indexing here to get
// wrong.
[[nodiscard]] std::vector<std::byte> encode_connect_request(const ConnectRequest& request);
[[nodiscard]] std::optional<ConnectRequest>
decode_connect_request(std::span<const std::byte> payload) noexcept;

[[nodiscard]] std::vector<std::byte> encode_connect_accept(const ConnectAccept& accept);
[[nodiscard]] std::optional<ConnectAccept>
decode_connect_accept(std::span<const std::byte> payload) noexcept;

[[nodiscard]] std::vector<std::byte> encode_connect_reject(const ConnectReject& reject);
[[nodiscard]] std::optional<ConnectReject>
decode_connect_reject(std::span<const std::byte> payload) noexcept;

[[nodiscard]] std::vector<std::byte> encode_disconnect(DisconnectReason reason);
[[nodiscard]] std::optional<DisconnectReason>
decode_disconnect(std::span<const std::byte> payload) noexcept;

[[nodiscard]] std::vector<std::byte> encode_heartbeat();
[[nodiscard]] bool decode_heartbeat(std::span<const std::byte> payload) noexcept;

} // namespace rime::net
