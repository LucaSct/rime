// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/stream/protocol.hpp"

#include <array>
#include <bit>
#include <optional>
#include <utility>

#include "rime/core/diagnostics/log.hpp"

// The protocol's implementation. The heart is a pair of little-endian byte cursors — everything
// else is "write these fields, read these fields." We serialize field-by-field (never memcpy a
// struct) so the wire bytes are identical on every compiler and CPU regardless of struct padding or
// host endianness; that portability is the whole point of a versioned protocol the editor will
// depend on.
namespace rime::stream {
namespace {

// Append little-endian integers to a growing byte buffer. Every multi-byte value is decomposed into
// bytes explicitly, so the result does not depend on the host's byte order.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& out) : out_(out) {}

    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }

    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFFu));
        u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    }

    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v & 0xFFFFu));
        u16(static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
    }

    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
        u32(static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
    }

    void i32(std::int32_t v) { u32(std::bit_cast<std::uint32_t>(v)); }

    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }

    void bytes(std::span<const std::byte> b) { out_.insert(out_.end(), b.begin(), b.end()); }

private:
    std::vector<std::byte>& out_;
};

// Read little-endian integers from a byte span, bounds-checked. Every reader returns false on
// underflow and advances nothing past the end, so a truncated or hostile buffer is a clean failure
// — never an out-of-bounds read.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> in) : in_(in) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }

    [[nodiscard]] bool u8(std::uint8_t& v) {
        if (remaining() < 1) {
            return false;
        }
        v = std::to_integer<std::uint8_t>(in_[pos_++]);
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& v) {
        std::uint8_t a = 0, b = 0;
        if (!u8(a) || !u8(b)) {
            return false;
        }
        v = static_cast<std::uint16_t>(static_cast<unsigned>(a) | (static_cast<unsigned>(b) << 8));
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& v) {
        std::uint16_t lo = 0, hi = 0;
        if (!u16(lo) || !u16(hi)) {
            return false;
        }
        v = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& v) {
        std::uint32_t lo = 0, hi = 0;
        if (!u32(lo) || !u32(hi)) {
            return false;
        }
        v = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        return true;
    }

    [[nodiscard]] bool i32(std::int32_t& v) {
        std::uint32_t u = 0;
        if (!u32(u)) {
            return false;
        }
        v = std::bit_cast<std::int32_t>(u);
        return true;
    }

    [[nodiscard]] bool f32(float& v) {
        std::uint32_t u = 0;
        if (!u32(u)) {
            return false;
        }
        v = std::bit_cast<float>(u);
        return true;
    }

    // Copy all not-yet-read bytes into `dst` (the variable-length tail, e.g. a frame's encoded
    // pixels).
    void take_rest(std::vector<std::byte>& dst) {
        dst.assign(in_.begin() + static_cast<std::ptrdiff_t>(pos_), in_.end());
        pos_ = in_.size();
    }

private:
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
};

// Stable wire codes for the pixel format, kept independent of rhi::Format's internal numbering —
// the long-lived protocol must not break if that enum is ever renumbered. Only the 4 formats the
// codecs speak have codes; anything else is rejected.
enum class WireFormat : std::uint8_t {
    RGBA8Unorm = 0,
    RGBA8Srgb = 1,
    BGRA8Unorm = 2,
    BGRA8Srgb = 3,
};

std::optional<std::uint8_t> wire_format_of(rhi::Format f) {
    switch (f) {
        case rhi::Format::RGBA8Unorm:
            return static_cast<std::uint8_t>(WireFormat::RGBA8Unorm);
        case rhi::Format::RGBA8Srgb:
            return static_cast<std::uint8_t>(WireFormat::RGBA8Srgb);
        case rhi::Format::BGRA8Unorm:
            return static_cast<std::uint8_t>(WireFormat::BGRA8Unorm);
        case rhi::Format::BGRA8Srgb:
            return static_cast<std::uint8_t>(WireFormat::BGRA8Srgb);
        default:
            return std::nullopt;
    }
}

std::optional<rhi::Format> rhi_format_of(std::uint8_t code) {
    switch (static_cast<WireFormat>(code)) {
        case WireFormat::RGBA8Unorm:
            return rhi::Format::RGBA8Unorm;
        case WireFormat::RGBA8Srgb:
            return rhi::Format::RGBA8Srgb;
        case WireFormat::BGRA8Unorm:
            return rhi::Format::BGRA8Unorm;
        case WireFormat::BGRA8Srgb:
            return rhi::Format::BGRA8Srgb;
        default:
            return std::nullopt;
    }
}

// Map a codec byte off the wire to the Codec enum — nullopt for values this build does not know.
// The single place the protocol's "which codecs exist" knowledge lives, so appending a codec
// (Av1 was s1.2's) is a one-line change here plus its enumerator.
std::optional<Codec> known_codec(std::uint8_t code) {
    switch (static_cast<Codec>(code)) {
        case Codec::Raw:
        case Codec::LZ4:
        case Codec::Jpeg:
        case Codec::Av1:
            return static_cast<Codec>(code);
    }
    return std::nullopt;
}

} // namespace

// ── FrameMessage ────────────────────────────────────────────────────────────────────────────────

void FrameMessage::encode(std::vector<std::byte>& out) const {
    out.clear();
    ByteWriter w(out);
    w.u64(sequence);
    w.u64(capture_us);
    w.u8(static_cast<std::uint8_t>(codec));
    // desc.format is always a codec-supported format here (the frame came from the codec, which
    // rejects others); map it, and fail loud rather than silently mislabel if that invariant
    // breaks.
    const auto wf = wire_format_of(desc.format);
    if (!wf) {
        RIME_ERROR("FrameMessage::encode: unsupported pixel format — wire frame will be malformed");
    }
    w.u8(wf.value_or(0));
    w.u32(desc.extent.width);
    w.u32(desc.extent.height);
    w.bytes(data);
}

bool FrameMessage::decode(std::span<const std::byte> payload) {
    ByteReader r(payload);
    std::uint8_t codec_byte = 0;
    std::uint8_t format_byte = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!r.u64(sequence) || !r.u64(capture_us) || !r.u8(codec_byte) || !r.u8(format_byte) ||
        !r.u32(width) || !r.u32(height)) {
        RIME_ERROR("FrameMessage::decode: truncated header ({} bytes)", payload.size());
        return false;
    }
    const auto known = known_codec(codec_byte);
    if (!known) {
        RIME_ERROR("FrameMessage::decode: unknown codec {}", codec_byte);
        return false;
    }
    const auto fmt = rhi_format_of(format_byte);
    if (!fmt) {
        RIME_ERROR("FrameMessage::decode: unknown pixel format {}", format_byte);
        return false;
    }
    codec = *known;
    desc.format = *fmt;
    desc.extent = {width, height};
    r.take_rest(data); // whatever remains is the encoded frame
    return true;
}

// ── CapabilitiesMessage / StreamConfigMessage / negotiation (s1.2, ADR-0030 §4) ─────────────────

void CapabilitiesMessage::encode(std::vector<std::byte>& out) const {
    out.clear();
    ByteWriter w(out);
    // A one-byte count caps the list at 255 — generous forever for codec *kinds* — and keeps the
    // message fixed-cost to parse.
    const auto count = static_cast<std::uint8_t>(decoders.size() > 255 ? 255 : decoders.size());
    w.u8(count);
    for (std::size_t i = 0; i < count; ++i) {
        w.u8(static_cast<std::uint8_t>(decoders[i]));
    }
}

bool CapabilitiesMessage::decode(std::span<const std::byte> payload) {
    decoders.clear();
    ByteReader r(payload);
    std::uint8_t count = 0;
    if (!r.u8(count)) {
        RIME_ERROR("CapabilitiesMessage::decode: truncated ({} bytes)", payload.size());
        return false;
    }
    decoders.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        std::uint8_t code = 0;
        if (!r.u8(code)) {
            RIME_ERROR("CapabilitiesMessage::decode: truncated codec list ({} of {})", i, count);
            decoders.clear();
            return false;
        }
        // Skip, don't reject: a codec value from the future is exactly what negotiation exists to
        // handle — we simply cannot pick it. Preference *order* among the survivors is preserved.
        if (const auto codec = known_codec(code)) {
            decoders.push_back(*codec);
        }
    }
    return true;
}

void StreamConfigMessage::encode(std::vector<std::byte>& out) const {
    out.clear();
    ByteWriter w(out);
    w.u8(static_cast<std::uint8_t>(codec));
    const auto wf = wire_format_of(desc.format);
    if (!wf) {
        RIME_ERROR("StreamConfigMessage::encode: unsupported pixel format — config malformed");
    }
    w.u8(wf.value_or(0));
    w.u32(desc.extent.width);
    w.u32(desc.extent.height);
    w.bytes(codec_config); // variable-length tail, like FrameMessage's pixels
}

bool StreamConfigMessage::decode(std::span<const std::byte> payload) {
    ByteReader r(payload);
    std::uint8_t codec_byte = 0;
    std::uint8_t format_byte = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!r.u8(codec_byte) || !r.u8(format_byte) || !r.u32(width) || !r.u32(height)) {
        RIME_ERROR("StreamConfigMessage::decode: truncated header ({} bytes)", payload.size());
        return false;
    }
    // Unlike Capabilities, an unknown codec here IS an error: the server *chose* it, so a client
    // that cannot name it can never decode the stream — fail at config, not at frame 0.
    const auto known = known_codec(codec_byte);
    if (!known) {
        RIME_ERROR("StreamConfigMessage::decode: unknown codec {}", codec_byte);
        return false;
    }
    const auto fmt = rhi_format_of(format_byte);
    if (!fmt) {
        RIME_ERROR("StreamConfigMessage::decode: unknown pixel format {}", format_byte);
        return false;
    }
    codec = *known;
    desc.format = *fmt;
    desc.extent = {width, height};
    r.take_rest(codec_config);
    return true;
}

std::optional<Codec> choose_codec(std::span<const Codec> client_preference,
                                  std::span<const Codec> server_supported) {
    for (const Codec want : client_preference) {
        for (const Codec have : server_supported) {
            if (want == have) {
                return want;
            }
        }
    }
    return std::nullopt;
}

// ── InputEvent ──────────────────────────────────────────────────────────────────────────────────

void InputEvent::encode(std::vector<std::byte>& out) const {
    out.clear();
    ByteWriter w(out);
    w.u8(static_cast<std::uint8_t>(kind));
    w.u32(code);
    w.i32(x);
    w.i32(y);
    w.f32(scroll_x);
    w.f32(scroll_y);
    w.u32(mods);
}

bool InputEvent::decode(std::span<const std::byte> payload) {
    ByteReader r(payload);
    std::uint8_t kind_byte = 0;
    if (!r.u8(kind_byte) || !r.u32(code) || !r.i32(x) || !r.i32(y) || !r.f32(scroll_x) ||
        !r.f32(scroll_y) || !r.u32(mods)) {
        RIME_ERROR("InputEvent::decode: truncated ({} bytes)", payload.size());
        return false;
    }
    if (kind_byte > static_cast<std::uint8_t>(Kind::PointerScroll)) {
        RIME_ERROR("InputEvent::decode: unknown kind {}", kind_byte);
        return false;
    }
    kind = static_cast<Kind>(kind_byte);
    return true;
}

// ── ProtocolConnection ──────────────────────────────────────────────────────────────────────────

ProtocolConnection::ProtocolConnection(platform::TcpSocket socket) noexcept
    : socket_(std::move(socket)) {}

bool ProtocolConnection::handshake() {
    // Send our Hello: [ magic:u32 ][ version:u16 ]. Both ends do this; the tiny 6-byte write never
    // blocks against the peer's, so a symmetric send-then-recv cannot deadlock.
    std::vector<std::byte> hello;
    ByteWriter w(hello);
    w.u32(kProtocolMagic);
    w.u16(kProtocolVersion);
    if (!socket_.send_all(hello)) {
        RIME_ERROR("protocol: failed to send handshake");
        return false;
    }

    std::array<std::byte, 6> buf{};
    if (!socket_.recv_exact(buf)) {
        RIME_ERROR("protocol: failed to read peer handshake (connection closed or error)");
        return false;
    }
    ByteReader r(buf);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    (void)r.u32(magic); // exactly 6 bytes present — reads cannot fail
    (void)r.u16(version);
    if (magic != kProtocolMagic) {
        RIME_ERROR("protocol: bad magic 0x{:08X} — not a Rime stream", magic);
        return false;
    }
    if (version != kProtocolVersion) {
        RIME_ERROR("protocol: peer version {} != ours {}", version, kProtocolVersion);
        return false;
    }
    return true;
}

bool ProtocolConnection::send_message(MessageType type, std::span<const std::byte> payload) {
    if (payload.size() > kMaxMessageBytes) {
        RIME_ERROR(
            "protocol: outgoing payload {} exceeds max {}", payload.size(), kMaxMessageBytes);
        return false;
    }
    // Envelope header: [ type:u16 ][ length:u32 ]. Sent separately from the payload — two writes,
    // no copy of a possibly-large frame. (Coalescing header+payload into one write is a later
    // tweak; TCP reassembles either way because the receiver reads exactly length bytes.)
    scratch_.clear();
    ByteWriter w(scratch_);
    w.u16(static_cast<std::uint16_t>(type));
    w.u32(static_cast<std::uint32_t>(payload.size()));
    if (!socket_.send_all(scratch_)) {
        RIME_ERROR("protocol: failed to send message header");
        return false;
    }
    if (!payload.empty() && !socket_.send_all(payload)) {
        RIME_ERROR("protocol: failed to send message payload");
        return false;
    }
    return true;
}

bool ProtocolConnection::send_frame(const FrameMessage& frame) {
    std::vector<std::byte> payload;
    frame.encode(payload);
    return send_message(MessageType::Frame, payload);
}

bool ProtocolConnection::send_input(const InputEvent& event) {
    std::vector<std::byte> payload;
    event.encode(payload);
    return send_message(MessageType::Input, payload);
}

bool ProtocolConnection::send_capabilities(const CapabilitiesMessage& caps) {
    std::vector<std::byte> payload;
    caps.encode(payload);
    return send_message(MessageType::Capabilities, payload);
}

bool ProtocolConnection::send_stream_config(const StreamConfigMessage& config) {
    std::vector<std::byte> payload;
    config.encode(payload);
    return send_message(MessageType::StreamConfig, payload);
}

bool ProtocolConnection::send_keyframe_request() {
    return send_message(MessageType::KeyframeRequest, {});
}

bool ProtocolConnection::send_bye() {
    return send_message(MessageType::Bye, {});
}

bool ProtocolConnection::recv_message(MessageType& type, std::vector<std::byte>& payload) {
    // A failed header read is the *normal* end of a stream (peer closed at a message boundary) as
    // well as the error path, so we do not log it — the caller's receive loop simply ends.
    // recv_exact already turns a mid-message EOF into a failure, so a truncated header is caught
    // here.
    std::array<std::byte, 6> header{};
    if (!socket_.recv_exact(header)) {
        return false;
    }
    ByteReader r(header);
    std::uint16_t type_raw = 0;
    std::uint32_t length = 0;
    (void)r.u16(type_raw); // 6 bytes present — cannot fail
    (void)r.u32(length);
    if (length > kMaxMessageBytes) {
        RIME_ERROR(
            "protocol: incoming length {} exceeds max {} — refusing", length, kMaxMessageBytes);
        return false;
    }
    type = static_cast<MessageType>(type_raw);
    payload.resize(length);
    if (length > 0 && !socket_.recv_exact(payload)) {
        RIME_ERROR("protocol: truncated payload (connection closed or error)");
        return false;
    }
    return true;
}

} // namespace rime::stream
