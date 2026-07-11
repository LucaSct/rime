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
} // namespace

TEST_CASE("FrameMessage round-trips through bytes") {
    stream::FrameMessage src;
    src.sequence = 0xDEADBEEFCAFEULL;
    src.capture_us = 1234567890ULL;
    src.codec = stream::Codec::Jpeg;
    src.desc = {{1280, 720}, rhi::Format::BGRA8Unorm};
    src.data = {b(1), b(2), b(3), b(250), b(0), b(255)}; // stand-in for encoded bytes

    Bytes payload;
    src.encode(payload);

    stream::FrameMessage dst;
    REQUIRE(dst.decode(payload));
    CHECK(dst.sequence == src.sequence);
    CHECK(dst.capture_us == src.capture_us);
    CHECK(dst.codec == stream::Codec::Jpeg);
    CHECK(dst.desc.extent.width == 1280);
    CHECK(dst.desc.extent.height == 720);
    CHECK(dst.desc.format == rhi::Format::BGRA8Unorm);
    CHECK(dst.data == src.data);
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
}

TEST_CASE("decode refuses malformed payloads instead of misreading") {
    SUBCASE("truncated frame header") {
        stream::FrameMessage m;
        CHECK_FALSE(m.decode(Bytes(4))); // header needs 26 bytes
    }
    SUBCASE("truncated input event") {
        stream::InputEvent e;
        CHECK_FALSE(e.decode(Bytes(3))); // needs 25 bytes
    }
    SUBCASE("frame with an unknown codec code") {
        // Valid-length header, but codec byte = 99. Build it by encoding a good frame then poking.
        stream::FrameMessage good;
        good.codec = stream::Codec::Raw;
        good.desc = {{2, 2}, rhi::Format::RGBA8Unorm};
        good.data = Bytes(2 * 2 * 4);
        Bytes payload;
        good.encode(payload);
        payload[16] = b(99); // codec byte sits after seq(8)+capture(8)
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
        payload[17] = b(200); // format byte follows the codec byte
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
