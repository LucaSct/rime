// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
//
// Version history:
//   1 — S0.4: handshake, Frame, Input, Bye.
//   2 — s1.2 (ADR-0030 §4): codec negotiation (Capabilities/StreamConfig), KeyframeRequest, and
//       Codec::Av1 on Frame messages. Incompatible because a v1 client would sit waiting for
//       frames it cannot decode; the version field exists so it is refused at connect instead.
//   3 — s1.3 (ADR-0030 §5): the latency ledger — Frame carries the server's per-stage stamps + the
//       echoed input seq/time, and Input carries a client stamp + sequence number. Wider payloads,
//       so a v2 peer would misread them; refused at connect.
inline constexpr std::uint32_t kProtocolMagic = 0x524D5331u; // 'R''M''S''1'
inline constexpr std::uint16_t kProtocolVersion = 3;

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
    // s1.2: stream parameters, sent before the first Frame and again on any codec/resolution
    // change. For a stateful codec (Av1) it carries the codec config a decoder needs *before*
    // frame 0 — the AV1 sequence header, the H.264 SPS/PPS analog. (StreamConfigMessage payload)
    StreamConfig = 0x0002,

    Input = 0x0101, // client -> server: one input event (InputEvent payload)
    // s1.2: the client's half of codec negotiation — the decoders it owns, in preference order,
    // sent once right after the handshake; the server picks (see choose_codec) and answers with
    // StreamConfig. A message rather than a fatter handshake so the 6-byte hello stays dumb and
    // version-gated. (CapabilitiesMessage payload)
    Capabilities = 0x0102,
    // s1.2: "start me a fresh delta chain" — an inter-frame decoder can only join (or recover) at
    // a keyframe, so a client sends this on connect/reset; S2's loss recovery reuses it. The
    // server answers by forcing an intra on its VideoEncoder. No payload.
    KeyframeRequest = 0x0103,

    // Reserved for the M9 editor channel (the 07-02 plan's M6 line item). The editor is a client of
    // a live engine process (ADR-0016); its viewport/command traffic will use message types in
    // [EditorReservedBegin, EditorReservedEnd]. M6 only RESERVES the range — no handler exists yet.
    // These two are range markers, not messages, so a future dispatcher can test membership.
    //
    // Forward-compatibility rule this reservation relies on: the envelope carries the raw u16 type
    // TRANSPARENTLY — recv_message() never rejects a type it doesn't recognize, it just hands the
    // raw value to the caller, who ignores anything it can't handle. So reserving a range costs
    // nothing today and breaks nothing: an old peer that receives a 0x02xx message simply drops it.
    // When the editor channel actually lands (M9), the handshake `version:u16` is bumped so a new
    // client only speaks 0x02xx to a new-enough server. This is pinned by a test in
    // tests/stream/protocol_test.
    EditorReservedBegin = 0x0200,
    EditorReservedEnd = 0x02FF,

    Bye = 0xFFFF, // either direction: graceful "I'm closing" (no payload)
};

// One encoded frame, ready for the wire. `data` is the codec's output (S0.3); `codec` + `desc` are
// everything the peer needs to decode it. `sequence` counts frames (gap detection). The remaining
// scalars are the **server half of the latency ledger** (s1.3, ADR-0030 §5), all in the server's
// monotonic clock: the per-stage stamps `capture_us`/`readback_us`/`encode_us`/`wire_us` — whose
// *differences* give the server-side stage costs with no clock sync — plus the echo of the most
// recent input the server has applied, `last_input_seq` and its client-clock send time
// `last_input_client_us`, so the client measures input-to-photon offset-free (see latency.hpp).
// Payload layout:
//   [ seq:u64 ][ capture_us:u64 ][ readback_us:u64 ][ encode_us:u64 ][ wire_us:u64 ]
//   [ last_input_seq:u32 ][ last_input_client_us:u64 ][ codec:u8 ][ fmt:u8 ][ w:u32 ][ h:u32 ][
//   data... ]
struct FrameMessage {
    std::uint64_t sequence = 0;
    std::uint64_t capture_us = 0;     // server clock: begin_capture submitted (ledger stage 1)
    std::uint64_t readback_us = 0;    // server clock: try_get_frame returned (stage 2)
    std::uint64_t encode_us = 0;      // server clock: codec packet ready (stage 3)
    std::uint64_t wire_us = 0;        // server clock: just before send (stage 4)
    std::uint32_t last_input_seq = 0; // most recent input applied (0 = none yet)
    std::uint64_t last_input_client_us = 0; // that input's client-clock send time (echoed back)
    Codec codec = Codec::Jpeg;
    ImageDesc desc{};
    std::vector<std::byte> data;

    // Serialize the payload (not the envelope) into `out` (cleared, then filled).
    void encode(std::vector<std::byte>& out) const;
    // Parse a payload produced by encode(). Returns false (logged) on truncation or an unsupported
    // codec/format code — never reads past `payload`.
    [[nodiscard]] bool decode(std::span<const std::byte> payload);
};

// The client's decoder inventory, most-preferred first — its half of codec negotiation (s1.2,
// ADR-0030 §4). Sent once, right after the handshake. Preference belongs to the *client* because
// only it knows its situation (a WAN client prefers Av1's bandwidth; the local editor prefers
// LZ4's losslessness); the server just picks the first entry it can encode. Payload layout:
//   [ count:u8 ][ codec:u8 x count ]
struct CapabilitiesMessage {
    std::vector<Codec> decoders;

    void encode(std::vector<std::byte>& out) const;
    // Forward-compatibility: codec values *we* don't recognize are skipped, not rejected — a
    // newer client advertising a future codec must still negotiate down to one we share. Only a
    // truncated payload is an error.
    [[nodiscard]] bool decode(std::span<const std::byte> payload);
};

// The server's answer: everything the client needs to stand up a decoder *before* the first
// frame arrives. For Av1 `codec_config` carries the encoder's sequence header (see
// VideoEncoder::sequence_header); for the stateless codecs it is empty. Re-sent whenever the
// stream's codec or geometry changes — the client then tears down and reopens its decoder.
// Payload layout:
//   [ codec:u8 ][ fmt:u8 ][ w:u32 ][ h:u32 ][ codec_config... ]
struct StreamConfigMessage {
    Codec codec = Codec::Jpeg;
    ImageDesc desc{};
    std::vector<std::byte> codec_config;

    void encode(std::vector<std::byte>& out) const;
    [[nodiscard]] bool decode(std::span<const std::byte> payload);
};

// The server's pick: the first codec in the client's preference list that the server supports —
// client preference decides *which* codec, the server list decides *whether*. Returns nullopt
// when the lists do not intersect (the server should then say Bye; better than silently
// streaming something the peer cannot decode).
[[nodiscard]] std::optional<Codec> choose_codec(std::span<const Codec> client_preference,
                                                std::span<const Codec> server_supported);

// One input event on its way back to the engine. A single tagged struct (rather than a struct per
// event) keeps the wire and the server's event-injection loop simple. Fields not relevant to a
// `kind` are zero. The client fills this from platform events; the server maps it back to platform
// input (S0.5). Payload layout (fixed 37 bytes):
//   [ kind:u8 ][ code:u32 ][ x:i32 ][ y:i32 ][ scroll_x:f32 ][ scroll_y:f32 ][ mods:u32 ]
//   [ client_us:u64 ][ seq:u32 ]
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
    // s1.3 (ADR-0030 §5): the input's client-clock send time + a per-client sequence number. The
    // server echoes seq + client_us on the frame that first reflects this input, closing the
    // offset-free input-to-photon measurement (latency.hpp). Old-style constructions leave them 0,
    // which the server reads as "un-timed" and simply does not echo.
    std::uint64_t client_us = 0;
    std::uint32_t seq = 0;

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
    [[nodiscard]] bool send_capabilities(const CapabilitiesMessage& caps);
    [[nodiscard]] bool send_stream_config(const StreamConfigMessage& config);
    [[nodiscard]] bool send_keyframe_request();
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
