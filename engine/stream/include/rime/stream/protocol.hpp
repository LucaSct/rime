// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/platform/socket.hpp"
#include "rime/stream/frame_codec.hpp"

// The **protocol** — the third brick of the graphics-streaming track (Track S / S0.4). The codec
// (S0.3) gave us small frame bytes; this defines how they, and input events coming back, are
// *framed* on the wire and carried over the blocking TCP sockets (S0.1). It is the seam that
// outlives S0 — the **M9 editor viewport rides this exact protocol** (ADR-0016) — so it is
// **versioned from day one**: a handshake exchanges a magic + version on connect, and the message
// space is meant to *grow* (editor edit/inspect messages at M6/M9), never to be renumbered.
//
// Wire shape (all integers **little-endian**, chosen once so the format is identical on every
// platform regardless of struct padding or host byte order — we serialize field-by-field, never
// memcpy a struct):
//
//   Handshake (once per side, right after connect):  [ magic:u32 ][ version:u16 ]
//   Every message after that:                        [ type:u16 ][ length:u32 ][ payload:length ]
//
// v0 is deliberately dumb: blocking, length-prefixed, one message at a time.
// Non-blocking/multiplexed I/O, compression negotiation, and TLS are later (S1–S2); the versioned
// header is what lets them arrive without breaking the editor that depends on this. Design:
// docs/design/graphics-streaming.md.
namespace rime::stream {

// Wire identity. The magic is ASCII "RMS1" (Rime Media Stream), so a wrong-protocol peer is
// rejected at the handshake instead of being misread. Bump kProtocolVersion for any incompatible
// wire change.
inline constexpr std::uint32_t kProtocolMagic = 0x524D5331u; // 'R''M''S''1'
inline constexpr std::uint16_t kProtocolVersion = 1;

// An upper bound on a single message's payload, so a corrupt or hostile length field can't make us
// try to allocate/read gigabytes. A raw 4K RGBA frame is ~33 MiB; 64 MiB leaves headroom while
// still bounding the blast radius. (JPEG frames are far smaller; this only ever bites a rogue
// peer.)
inline constexpr std::uint32_t kMaxMessageBytes = 64u * 1024u * 1024u;

// What a framed message carries. The space is split by direction purely by convention (the
// transport does not enforce it): Frame flows server -> client, Input flows client -> server.
// Values are wire constants — append, never renumber.
enum class MessageType : std::uint16_t {
    Frame = 0x0001, // server -> client: one encoded video frame (FrameMessage payload)
    Input = 0x0101, // client -> server: one input event (InputEvent payload)
    Bye = 0xFFFF,   // either direction: graceful "I'm closing" (no payload)
};

// One encoded frame, ready for the wire. `data` is the codec's output (S0.3); `codec` + `desc` are
// everything the peer needs to decode it. `sequence` counts frames (gap detection later);
// `capture_us` is a monotonic capture timestamp for latency measurement (exact on loopback;
// cross-machine latency needs clock sync — an S1 concern). Payload layout:
//   [ seq:u64 ][ cap_us:u64 ][ codec:u8 ][ fmt:u8 ][ w:u32 ][ h:u32 ][ data... ]
struct FrameMessage {
    std::uint64_t sequence = 0;
    std::uint64_t capture_us = 0;
    Codec codec = Codec::Jpeg;
    ImageDesc desc{};
    std::vector<std::byte> data;

    // Serialize the payload (not the envelope) into `out` (cleared, then filled).
    void encode(std::vector<std::byte>& out) const;
    // Parse a payload produced by encode(). Returns false (logged) on truncation or an unsupported
    // codec/format code — never reads past `payload`.
    [[nodiscard]] bool decode(std::span<const std::byte> payload);
};

// One input event on its way back to the engine. A single tagged struct (rather than a struct per
// event) keeps the wire and the server's event-injection loop simple. Fields not relevant to a
// `kind` are zero. The client fills this from platform events; the server maps it back to platform
// input (S0.5). Payload layout (fixed 25 bytes):
//   [ kind:u8 ][ code:u32 ][ x:i32 ][ y:i32 ][ scroll_x:f32 ][ scroll_y:f32 ][ mods:u32 ]
struct InputEvent {
    enum class Kind : std::uint8_t {
        KeyDown = 0,
        KeyUp = 1,
        PointerMove = 2,
        PointerDown = 3,
        PointerUp = 4,
        PointerScroll = 5,
    };

    Kind kind = Kind::KeyDown;
    std::uint32_t code = 0; // Key*: a key code; PointerDown/Up: a button index
    std::int32_t x = 0;     // pointer position (PointerMove/Down/Up), in client pixels
    std::int32_t y = 0;     //
    float scroll_x = 0.0f;  // PointerScroll deltas
    float scroll_y = 0.0f;  //
    std::uint32_t mods = 0; // modifier-key bitmask (client-defined; carried verbatim)

    void encode(std::vector<std::byte>& out) const;
    [[nodiscard]] bool decode(std::span<const std::byte> payload);
};

// A framed message stream over one blocking TcpSocket. Owns the socket (move-only, RAII). This is
// what both ends hold: the server wraps an accepted connection, the client wraps its connected
// socket, both call handshake() once, then trade messages.
//
// Threading: not thread-safe for concurrent *senders* or concurrent *receivers*, but **one sender
// thread and one receiver thread may use it at the same time** — the send path and the recv path
// share no mutable state but the socket handle, and TCP is full-duplex. That is exactly what a
// streaming server needs: pump frames on one thread while draining input on another (see
// samples/04-remote-view).
class ProtocolConnection {
public:
    explicit ProtocolConnection(platform::TcpSocket socket) noexcept;

    ProtocolConnection(ProtocolConnection&&) noexcept = default;
    ProtocolConnection& operator=(ProtocolConnection&&) noexcept = default;
    ProtocolConnection(const ProtocolConnection&) = delete;
    ProtocolConnection& operator=(const ProtocolConnection&) = delete;

    // Exchange and validate the version header: send our Hello, read the peer's, check
    // magic+version. Call once, immediately after connecting/accepting. Returns false (logged) on
    // I/O error or a magic/version mismatch (a wrong-protocol or wrong-version peer is refused, not
    // misparsed).
    [[nodiscard]] bool handshake();

    // Send one framed message. `payload` may be empty (e.g. Bye). Returns false on I/O error or if
    // the payload exceeds kMaxMessageBytes.
    [[nodiscard]] bool send_message(MessageType type, std::span<const std::byte> payload);

    // Convenience wrappers that serialize a typed message and send it.
    [[nodiscard]] bool send_frame(const FrameMessage& frame);
    [[nodiscard]] bool send_input(const InputEvent& event);
    [[nodiscard]] bool send_bye();

    // Receive exactly one framed message: fills `type`, and `payload` (cleared then filled with the
    // message's payload bytes). Returns false on I/O error, on a clean peer close (EOF) before a
    // full message, or on a length that exceeds kMaxMessageBytes. The caller dispatches on `type`
    // and calls FrameMessage/InputEvent::decode on `payload`.
    [[nodiscard]] bool recv_message(MessageType& type, std::vector<std::byte>& payload);

    [[nodiscard]] bool is_open() const noexcept { return socket_.is_open(); }

private:
    platform::TcpSocket socket_;
    std::vector<std::byte> scratch_; // reused envelope-serialization buffer (send path)
};

} // namespace rime::stream
