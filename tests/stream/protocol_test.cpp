// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the S0.4 protocol. Two halves, both **GPU-free** (they run on every CI OS):
//   1. Serialization round-trips — every message encodes to bytes and decodes back unchanged, and a
//      truncated or malformed payload is refused rather than misread.
//   2. A loopback integration test over real TCP (127.0.0.1, ephemeral port): the two ends shake
//      hands, the client sends input events, the server sends a JPEG-encoded frame, and the client
//      decodes it back to pixels — codec + protocol + sockets end to end, no device needed.
//
// doctest's macros are not thread-safe, so (as in the platform socket test) the client runs on a
// thread that only *records* its outcome; join() publishes it and the main thread does the
// asserting.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rime/platform/socket.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/protocol.hpp"

using namespace rime;

namespace {
using Bytes = std::vector<std::byte>;

std::byte b(int v) {
    return static_cast<std::byte>(static_cast<std::uint8_t>(v));
}

int u8(std::byte x) {
    return static_cast<int>(std::to_integer<std::uint8_t>(x));
}

// A unique socket path in the system temp dir for a local-socket test. Random-suffixed so parallel
// test binaries don't collide; kept short so it stays under AF_UNIX's ~108-byte sun_path limit even
// under a longer temp dir (macOS /var/folders, Windows AppData\Local\Temp).
std::string unique_local_path(const char* tag) {
    std::random_device rd;
    const std::string name = "rime-s14-" + std::string(tag) + "-" + std::to_string(rd()) + ".sock";
    return (std::filesystem::temp_directory_path() / name).string();
}
} // namespace

TEST_CASE("FrameMessage round-trips through bytes") {
    stream::FrameMessage src;
    src.sequence = 0xDEADBEEFCAFEULL;
    src.capture_us = 1234567890ULL;
    src.readback_us = 1234567930ULL; // s1.3 ledger stamps (server clock)
    src.encode_us = 1234567945ULL;
    src.wire_us = 1234567950ULL;
    src.last_input_seq = 4242;              // echoed input identity
    src.last_input_client_us = 55500000ULL; // echoed client-clock send time
    src.codec = stream::Codec::Jpeg;
    src.desc = {{1280, 720}, rhi::Format::BGRA8Unorm};
    src.data = {b(1), b(2), b(3), b(250), b(0), b(255)}; // stand-in for encoded bytes

    Bytes payload;
    src.encode(payload);

    stream::FrameMessage dst;
    REQUIRE(dst.decode(payload));
    CHECK(dst.sequence == src.sequence);
    CHECK(dst.capture_us == src.capture_us);
    CHECK(dst.readback_us == src.readback_us);
    CHECK(dst.encode_us == src.encode_us);
    CHECK(dst.wire_us == src.wire_us);
    CHECK(dst.last_input_seq == src.last_input_seq);
    CHECK(dst.last_input_client_us == src.last_input_client_us);
    CHECK(dst.codec == stream::Codec::Jpeg);
    CHECK(dst.desc.extent.width == 1280);
    CHECK(dst.desc.extent.height == 720);
    CHECK(dst.desc.format == rhi::Format::BGRA8Unorm);
    CHECK(dst.data == src.data);
}

TEST_CASE("CapabilitiesMessage round-trips a decoder preference list (s1.2)") {
    stream::CapabilitiesMessage src;
    src.decoders = {stream::Codec::Av1, stream::Codec::Jpeg, stream::Codec::LZ4};
    Bytes payload;
    src.encode(payload);

    stream::CapabilitiesMessage dst;
    REQUIRE(dst.decode(payload));
    CHECK(dst.decoders == src.decoders); // order (preference) preserved

    SUBCASE("an unknown codec code is skipped, not rejected (forward-compat)") {
        // Poke a future codec (0x7F) into the second slot: negotiation must still parse the rest.
        payload[2] = b(0x7F);
        stream::CapabilitiesMessage fwd;
        REQUIRE(fwd.decode(payload));
        CHECK(fwd.decoders.size() == 2); // the unknown entry dropped; the two known survive
        CHECK(fwd.decoders[0] == stream::Codec::Av1);
        CHECK(fwd.decoders[1] == stream::Codec::LZ4);
    }
    SUBCASE("a truncated codec list is refused") {
        stream::CapabilitiesMessage bad;
        Bytes truncated = {b(3), b(0), b(1)}; // claims 3 codecs, supplies 2
        CHECK_FALSE(bad.decode(truncated));
    }
}

TEST_CASE("StreamConfigMessage round-trips codec + geometry + AV1 sequence header (s1.2)") {
    stream::StreamConfigMessage src;
    src.codec = stream::Codec::Av1;
    src.desc = {{640, 360}, rhi::Format::RGBA8Unorm};
    src.codec_config = {b(0x0A), b(0x0B), b(0x0C), b(0xFF)}; // stand-in sequence-header bytes
    Bytes payload;
    src.encode(payload);

    stream::StreamConfigMessage dst;
    REQUIRE(dst.decode(payload));
    CHECK(dst.codec == stream::Codec::Av1);
    CHECK(dst.desc.extent.width == 640);
    CHECK(dst.desc.extent.height == 360);
    CHECK(dst.desc.format == rhi::Format::RGBA8Unorm);
    CHECK(dst.codec_config == src.codec_config);

    SUBCASE("truncated header is refused") {
        stream::StreamConfigMessage bad;
        CHECK_FALSE(bad.decode(Bytes(4))); // header needs 10 bytes
    }
    SUBCASE("an unknown chosen codec is refused (server picked something we can't decode)") {
        payload[0] = b(0x7F);
        stream::StreamConfigMessage bad;
        CHECK_FALSE(bad.decode(payload));
    }
}

TEST_CASE("choose_codec honours client preference within the server's support (s1.2)") {
    const stream::Codec server[] = {stream::Codec::Av1, stream::Codec::Jpeg, stream::Codec::LZ4};

    SUBCASE("client's top shared choice wins") {
        const stream::Codec client[] = {stream::Codec::Av1, stream::Codec::Jpeg};
        const auto picked = stream::choose_codec(client, server);
        REQUIRE(picked.has_value());
        CHECK(*picked == stream::Codec::Av1);
    }
    SUBCASE("client preference — not server order — decides among shared codecs") {
        const stream::Codec client[] = {stream::Codec::LZ4, stream::Codec::Av1};
        const auto picked = stream::choose_codec(client, server);
        REQUIRE(picked.has_value());
        CHECK(*picked == stream::Codec::LZ4); // even though the server lists Av1 first
    }
    SUBCASE("no intersection yields nullopt (server should then Bye)") {
        const stream::Codec client[] = {stream::Codec::Raw};
        CHECK_FALSE(stream::choose_codec(client, server).has_value());
    }
}

TEST_CASE("InputEvent round-trips every field, including signed and float") {
    auto roundtrip = [](const stream::InputEvent& src) {
        Bytes payload;
        src.encode(payload);
        stream::InputEvent dst;
        REQUIRE(dst.decode(payload));
        return dst;
    };

    SUBCASE("key event carries code + mods") {
        stream::InputEvent src;
        src.kind = stream::InputEvent::Kind::KeyDown;
        src.code = 0x41; // 'A'
        src.mods = 0x5;
        const auto dst = roundtrip(src);
        CHECK(dst.kind == stream::InputEvent::Kind::KeyDown);
        CHECK(dst.code == 0x41u);
        CHECK(dst.mods == 0x5u);
    }
    SUBCASE("pointer motion carries negative coordinates exactly") {
        stream::InputEvent src;
        src.kind = stream::InputEvent::Kind::PointerMove;
        src.x = -7;
        src.y = 480;
        const auto dst = roundtrip(src);
        CHECK(dst.kind == stream::InputEvent::Kind::PointerMove);
        CHECK(dst.x == -7);
        CHECK(dst.y == 480);
    }
    SUBCASE("scroll carries floats bit-exact") {
        stream::InputEvent src;
        src.kind = stream::InputEvent::Kind::PointerScroll;
        src.scroll_x = -1.5f;
        src.scroll_y = 0.25f;
        const auto dst = roundtrip(src);
        CHECK(dst.scroll_x == -1.5f); // powers-of-two halves are exact in binary32
        CHECK(dst.scroll_y == 0.25f);
    }
    SUBCASE("s1.3 carries the client timestamp + sequence number") {
        stream::InputEvent src;
        src.kind = stream::InputEvent::Kind::PointerDown;
        src.code = 1;
        src.client_us = 0x0123456789ABCDEFULL;
        src.seq = 0xCAFEBABE;
        const auto dst = roundtrip(src);
        CHECK(dst.client_us == src.client_us);
        CHECK(dst.seq == src.seq);
    }
}

TEST_CASE("decode refuses malformed payloads instead of misreading") {
    SUBCASE("truncated frame header") {
        stream::FrameMessage m;
        CHECK_FALSE(m.decode(Bytes(4))); // header needs 62 bytes (s1.3 ledger stamps widened it)
    }
    SUBCASE("truncated input event") {
        stream::InputEvent e;
        CHECK_FALSE(e.decode(Bytes(3))); // needs 37 bytes (s1.3 client stamp + seq)
    }
    SUBCASE("frame with an unknown codec code") {
        // Valid-length header, but codec byte = 99. Build it by encoding a good frame then poking.
        stream::FrameMessage good;
        good.codec = stream::Codec::Raw;
        good.desc = {{2, 2}, rhi::Format::RGBA8Unorm};
        good.data = Bytes(2 * 2 * 4);
        Bytes payload;
        good.encode(payload);
        // codec byte follows
        // seq(8)+capture(8)+readback(8)+encode(8)+wire(8)+last_seq(4)+last_us(8).
        payload[52] = b(99);
        stream::FrameMessage m;
        CHECK_FALSE(m.decode(payload));
    }
    SUBCASE("frame with an unknown pixel-format code") {
        stream::FrameMessage good;
        good.codec = stream::Codec::Raw;
        good.desc = {{2, 2}, rhi::Format::RGBA8Unorm};
        good.data = Bytes(2 * 2 * 4);
        Bytes payload;
        good.encode(payload);
        payload[53] = b(200); // format byte follows the codec byte (offset 52)
        stream::FrameMessage m;
        CHECK_FALSE(m.decode(payload));
    }
}

// ── Loopback integration: handshake + input up, a JPEG frame down ──

TEST_CASE("loopback: handshake, client input, server frame, end to end") {
    auto listener = platform::TcpListener::bind(0); // ephemeral loopback port
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->local_port();
    REQUIRE(port != 0);

    // What the client will send up and what it manages to receive back.
    struct ClientOutcome {
        bool handshook = false;
        bool sent_inputs = false;
        bool got_frame = false;
        bool decoded = false;
        std::uint64_t seq = 0;
        int px_r = -1, px_g = -1, px_b = -1;
    } out;

    const stream::ImageDesc frame_desc{{32, 24}, rhi::Format::RGBA8Unorm};
    const int expect_r = 200, expect_g = 60, expect_b = 90;

    std::thread client([&] {
        auto sock = platform::TcpSocket::connect("127.0.0.1", port);
        if (!sock) {
            return;
        }
        stream::ProtocolConnection conn(std::move(*sock));
        if (!conn.handshake()) {
            return;
        }
        out.handshook = true;

        stream::InputEvent key;
        key.kind = stream::InputEvent::Kind::KeyDown;
        key.code = 0x20; // space
        stream::InputEvent move;
        move.kind = stream::InputEvent::Kind::PointerMove;
        move.x = 12;
        move.y = 8;
        out.sent_inputs = conn.send_input(key) && conn.send_input(move);
        if (!out.sent_inputs) {
            return;
        }

        // Now block for the server's frame, decode it back to pixels.
        stream::MessageType type{};
        Bytes payload;
        if (!conn.recv_message(type, payload) || type != stream::MessageType::Frame) {
            return;
        }
        out.got_frame = true;
        stream::FrameMessage fm;
        if (!fm.decode(payload)) {
            return;
        }
        out.seq = fm.sequence;
        Bytes pixels(fm.desc.byte_size());
        stream::FrameDecoder dec;
        if (!dec.decode(fm.codec, fm.desc, fm.data, pixels)) {
            return;
        }
        out.decoded = true;
        const std::size_t p = (8u * fm.desc.extent.width + 12u) * 4u; // an interior pixel
        out.px_r = u8(pixels[p + 0]);
        out.px_g = u8(pixels[p + 1]);
        out.px_b = u8(pixels[p + 2]);
    });

    // ── Server side, on the main thread (safe to assert here) ──
    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server(std::move(*accepted));
    REQUIRE(server.handshake());

    // Receive the two input events the client sent, in order.
    stream::MessageType type{};
    Bytes payload;
    REQUIRE(server.recv_message(type, payload));
    CHECK(type == stream::MessageType::Input);
    stream::InputEvent got_key;
    REQUIRE(got_key.decode(payload));
    CHECK(got_key.kind == stream::InputEvent::Kind::KeyDown);
    CHECK(got_key.code == 0x20u);

    REQUIRE(server.recv_message(type, payload));
    stream::InputEvent got_move;
    REQUIRE(got_move.decode(payload));
    CHECK(got_move.kind == stream::InputEvent::Kind::PointerMove);
    CHECK(got_move.x == 12);
    CHECK(got_move.y == 8);

    // Encode a known solid frame and stream it down.
    Bytes src(frame_desc.byte_size());
    for (std::size_t i = 0; i < src.size(); i += 4) {
        src[i + 0] = b(expect_r);
        src[i + 1] = b(expect_g);
        src[i + 2] = b(expect_b);
        src[i + 3] = b(255);
    }
    stream::FrameEncoder enc;
    stream::FrameMessage fm;
    fm.sequence = 42;
    fm.codec = stream::Codec::Jpeg;
    fm.desc = frame_desc;
    REQUIRE(enc.encode(stream::Codec::Jpeg, frame_desc, src, fm.data));
    REQUIRE(server.send_frame(fm));

    client.join(); // publishes `out`

    CHECK(out.handshook);
    CHECK(out.sent_inputs);
    CHECK(out.got_frame);
    CHECK(out.decoded);
    CHECK(out.seq == 42u);
    // JPEG is lossy, so allow a few LSB of slack on the reconstructed solid colour.
    CHECK(std::abs(out.px_r - expect_r) < 8);
    CHECK(std::abs(out.px_g - expect_g) < 8);
    CHECK(std::abs(out.px_b - expect_b) < 8);
}

// ── Local-socket transport (S1.4): the same protocol over a Unix-domain socket ──

TEST_CASE("loopback over a local socket: handshake + input + an LZ4 frame, bit-exact (s1.4)") {
    // The editor's wire: the exact same ProtocolConnection, over a LocalSocket instead of TCP. The
    // local default is LZ4 (lossless), so the far end must match the source **byte for byte** — the
    // S0.6 loopback assertion, now on the UDS path (CI's first exercise of the local transport).
    const std::string path = unique_local_path("uds");
    auto listener = platform::LocalListener::bind(path);
    REQUIRE(listener.has_value());

    struct ClientOutcome {
        bool handshook = false, sent = false, got_frame = false, decoded = false, exact = false;
        std::uint64_t seq = 0;
    } out;

    const stream::ImageDesc frame_desc{{32, 24}, rhi::Format::RGBA8Unorm};
    // A gradient a lossy codec would blur — but LZ4 is lossless, so it must come back exactly.
    Bytes src(frame_desc.byte_size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        src[i] = b(static_cast<int>((i * 7 + 3) & 0xFF));
    }

    std::thread client([&] {
        auto sock = platform::LocalSocket::connect(path);
        if (!sock) {
            return;
        }
        stream::ProtocolConnection conn(std::move(*sock));
        if (!conn.handshake()) {
            return;
        }
        out.handshook = true;
        stream::InputEvent e;
        e.kind = stream::InputEvent::Kind::PointerMove;
        e.x = 5;
        e.y = 6;
        e.seq = 1;
        e.client_us = 12345; // s1.3 fields ride the local wire too
        out.sent = conn.send_input(e);
        if (!out.sent) {
            return;
        }
        stream::MessageType type{};
        Bytes payload;
        if (!conn.recv_message(type, payload) || type != stream::MessageType::Frame) {
            return;
        }
        out.got_frame = true;
        stream::FrameMessage fm;
        if (!fm.decode(payload)) {
            return;
        }
        out.seq = fm.sequence;
        Bytes pixels(fm.desc.byte_size());
        stream::FrameDecoder dec;
        if (!dec.decode(fm.codec, fm.desc, fm.data, pixels)) {
            return;
        }
        out.decoded = true;
        out.exact = (pixels == src); // lossless: every byte
    });

    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server(std::move(*accepted));
    REQUIRE(server.handshake());

    stream::MessageType type{};
    Bytes payload;
    REQUIRE(server.recv_message(type, payload));
    CHECK(type == stream::MessageType::Input);
    stream::InputEvent got;
    REQUIRE(got.decode(payload));
    CHECK(got.seq == 1u);
    CHECK(got.client_us == 12345u);

    stream::FrameEncoder enc;
    stream::FrameMessage fm;
    fm.sequence = 7;
    fm.codec = stream::Codec::LZ4; // the local/editor default: exact, not merely close
    fm.desc = frame_desc;
    REQUIRE(enc.encode(stream::Codec::LZ4, frame_desc, src, fm.data));
    REQUIRE(server.send_frame(fm));

    client.join();

    CHECK(out.handshook);
    CHECK(out.sent);
    CHECK(out.got_frame);
    CHECK(out.decoded);
    CHECK(out.seq == 7u);
    CHECK(out.exact); // bit-exact over the local wire
}

TEST_CASE("LocalSocket / LocalListener refuse bad paths cleanly (s1.4)") {
    SUBCASE("an over-long path is refused, not truncated") {
        const std::string too_long(300, 'x'); // well past AF_UNIX's ~108-byte sun_path
        CHECK_FALSE(platform::LocalListener::bind(too_long).has_value());
        CHECK_FALSE(platform::LocalSocket::connect(too_long).has_value());
    }
    SUBCASE("connecting where no server is bound fails cleanly (the common 'not up yet')") {
        CHECK_FALSE(platform::LocalSocket::connect(unique_local_path("noserver")).has_value());
    }
    SUBCASE("bind creates the socket node and close() unlinks it") {
        const std::string path = unique_local_path("unlink");
        {
            auto l = platform::LocalListener::bind(path);
            REQUIRE(l.has_value());
            CHECK(std::filesystem::exists(path)); // the bind created the node
        } // ~LocalListener → close() unlinks
        CHECK_FALSE(std::filesystem::exists(path));
    }
}

TEST_CASE("a reserved editor message type transits transparently (forward-compat pin, M6.9)") {
    // The M9 editor channel will use message types in [EditorReservedBegin, EditorReservedEnd].
    // Reserving that range is only safe because the protocol carries an unknown type ID unmodified:
    // a message sent with a reserved type must round-trip as its exact raw value and be distinct
    // from every type this build handles — so today's peers ignore it, and M9 adds a handler by
    // bumping the handshake version. Range sanity first, then the wire round-trip over loopback.
    static_assert(static_cast<std::uint16_t>(stream::MessageType::EditorReservedBegin) == 0x0200);
    CHECK(stream::MessageType::EditorReservedBegin != stream::MessageType::Frame);
    CHECK(stream::MessageType::EditorReservedEnd != stream::MessageType::Bye);

    auto listener = platform::TcpListener::bind(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->local_port();
    REQUIRE(port != 0);

    const auto reserved = static_cast<stream::MessageType>(0x0200);
    const Bytes body{std::byte{1}, std::byte{2}, std::byte{3}};

    std::thread client([&] {
        auto sock = platform::TcpSocket::connect("127.0.0.1", port);
        if (!sock) {
            return;
        }
        stream::ProtocolConnection conn(std::move(*sock));
        if (!conn.handshake()) {
            return;
        }
        (void)conn.send_message(reserved, body);
    });

    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server(std::move(*accepted));
    REQUIRE(server.handshake());

    stream::MessageType type{};
    Bytes payload;
    REQUIRE(server.recv_message(type, payload));
    CHECK(type == reserved);                   // the raw reserved value survived the round-trip
    CHECK(type != stream::MessageType::Frame); // and today it reads as "unknown" to the dispatch
    CHECK(type != stream::MessageType::Input);
    CHECK(payload == body);

    client.join();
}

TEST_CASE("handshake rejects a peer speaking gibberish") {
    auto listener = platform::TcpListener::bind(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->local_port();

    std::thread client([&] {
        auto sock = platform::TcpSocket::connect("127.0.0.1", port);
        if (!sock) {
            return;
        }
        // Send 6 bytes that are not a valid Hello (wrong magic), then let the socket close.
        const std::array<std::byte, 6> junk{b(0), b(1), b(2), b(3), b(4), b(5)};
        (void)sock->send_all(junk);
    });

    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server(std::move(*accepted));
    CHECK_FALSE(server.handshake()); // bad magic -> handshake fails, not a crash

    client.join();
}
