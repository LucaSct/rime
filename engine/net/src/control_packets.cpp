// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/net/control_packets.hpp"

#include "rime/core/byte_cursor.hpp"

namespace rime::net {

namespace {

// Shared prologue so every decoder enforces the same two gates in the same order before touching a
// payload field: right tag, right magic. A failure is a clean drop — the caller counts it and moves
// on; nothing on the decode path allocates, throws, or logs on hostile input.
[[nodiscard]] bool read_header(core::ByteReader& reader, ControlTag expected) noexcept {
    std::uint8_t tag = 0;
    std::uint16_t magic = 0;
    return reader.u8(tag) && tag == static_cast<std::uint8_t>(expected) && reader.u16(magic) &&
           magic == kControlMagic;
}

void write_header(core::ByteWriter& writer, ControlTag tag) {
    writer.u8(static_cast<std::uint8_t>(tag));
    writer.u16(kControlMagic);
}

} // namespace

std::string_view to_string(DisconnectReason reason) noexcept {
    switch (reason) {
        case DisconnectReason::Graceful:
            return "graceful disconnect";
        case DisconnectReason::LocalClose:
            return "local close";
        case DisconnectReason::Timeout:
            return "timeout (peer silent)";
        case DisconnectReason::Replaced:
            return "replaced by a new incarnation";
        case DisconnectReason::ConnectTimeout:
            return "no answer to connect request";
        case DisconnectReason::ProtocolMismatch:
            return "protocol version mismatch";
        case DisconnectReason::AppMismatch:
            return "app id mismatch";
        case DisconnectReason::SchemaMismatch:
            return "schema hash mismatch";
        case DisconnectReason::ServerFull:
            return "server full";
    }
    return "unknown"; // unreachable for in-range values; keeps the switch total without a throw
}

std::vector<std::byte> encode_connect_request(const ConnectRequest& request) {
    std::vector<std::byte> payload;
    payload.reserve(21);
    core::ByteWriter writer(payload);
    write_header(writer, ControlTag::ConnectRequest);
    writer.u16(request.protocol);
    writer.u32(request.app_id);
    writer.u64(request.schema_hash);
    writer.u32(request.client_salt);
    return payload;
}

std::optional<ConnectRequest> decode_connect_request(std::span<const std::byte> payload) noexcept {
    core::ByteReader reader(payload);
    if (!read_header(reader, ControlTag::ConnectRequest)) {
        return std::nullopt;
    }
    ConnectRequest request;
    if (!reader.u16(request.protocol) || !reader.u32(request.app_id) ||
        !reader.u64(request.schema_hash) || !reader.u32(request.client_salt)) {
        return std::nullopt;
    }
    if (reader.remaining() != 0) {
        return std::nullopt; // trailing bytes = corruption, see the header's extension note
    }
    return request;
}

std::vector<std::byte> encode_connect_accept(const ConnectAccept& accept) {
    std::vector<std::byte> payload;
    payload.reserve(11);
    core::ByteWriter writer(payload);
    write_header(writer, ControlTag::ConnectAccept);
    writer.u32(accept.client_salt);
    writer.u32(accept.server_salt);
    return payload;
}

std::optional<ConnectAccept> decode_connect_accept(std::span<const std::byte> payload) noexcept {
    core::ByteReader reader(payload);
    if (!read_header(reader, ControlTag::ConnectAccept)) {
        return std::nullopt;
    }
    ConnectAccept accept;
    if (!reader.u32(accept.client_salt) || !reader.u32(accept.server_salt)) {
        return std::nullopt;
    }
    if (reader.remaining() != 0) {
        return std::nullopt;
    }
    return accept;
}

std::vector<std::byte> encode_connect_reject(const ConnectReject& reject) {
    std::vector<std::byte> payload;
    payload.reserve(24);
    core::ByteWriter writer(payload);
    write_header(writer, ControlTag::ConnectReject);
    writer.u32(reject.client_salt);
    writer.u8(static_cast<std::uint8_t>(reject.reason));
    writer.u64(reject.expected);
    writer.u64(reject.actual);
    return payload;
}

std::optional<ConnectReject> decode_connect_reject(std::span<const std::byte> payload) noexcept {
    core::ByteReader reader(payload);
    if (!read_header(reader, ControlTag::ConnectReject)) {
        return std::nullopt;
    }
    ConnectReject reject;
    std::uint8_t raw_reason = 0;
    if (!reader.u32(reject.client_salt) || !reader.u8(raw_reason) || !reader.u64(reject.expected) ||
        !reader.u64(reject.actual)) {
        return std::nullopt;
    }
    if (reader.remaining() != 0) {
        return std::nullopt;
    }
    // Only the four reject reasons are meaningful on this packet; anything else is corruption, not
    // a new policy we should silently forward to the game.
    switch (static_cast<DisconnectReason>(raw_reason)) {
        case DisconnectReason::ProtocolMismatch:
        case DisconnectReason::AppMismatch:
        case DisconnectReason::SchemaMismatch:
        case DisconnectReason::ServerFull:
            reject.reason = static_cast<DisconnectReason>(raw_reason);
            return reject;
        default:
            return std::nullopt;
    }
}

std::vector<std::byte> encode_disconnect(DisconnectReason reason) {
    std::vector<std::byte> payload;
    payload.reserve(4);
    core::ByteWriter writer(payload);
    write_header(writer, ControlTag::Disconnect);
    writer.u8(static_cast<std::uint8_t>(reason));
    return payload;
}

std::optional<DisconnectReason> decode_disconnect(std::span<const std::byte> payload) noexcept {
    core::ByteReader reader(payload);
    if (!read_header(reader, ControlTag::Disconnect)) {
        return std::nullopt;
    }
    std::uint8_t raw_reason = 0;
    if (!reader.u8(raw_reason) || reader.remaining() != 0) {
        return std::nullopt;
    }
    if (raw_reason > static_cast<std::uint8_t>(DisconnectReason::ServerFull)) {
        return std::nullopt;
    }
    return static_cast<DisconnectReason>(raw_reason);
}

std::vector<std::byte> encode_heartbeat() {
    std::vector<std::byte> payload;
    payload.reserve(3);
    core::ByteWriter writer(payload);
    write_header(writer, ControlTag::Heartbeat);
    return payload;
}

bool decode_heartbeat(std::span<const std::byte> payload) noexcept {
    core::ByteReader reader(payload);
    return read_header(reader, ControlTag::Heartbeat) && reader.remaining() == 0;
}

} // namespace rime::net
