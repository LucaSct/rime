// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// The Track-S engine-side **capstone**: the whole streaming pipe, end to end, over a real socket.
// Where frame_codec_test and protocol_test used CPU-synthesized frames, this one starts from a
// *actually rendered* frame — so it proves S0.2 → S0.3 → S0.4 connect:
//
//   render off-screen (RHI) -> FrameStreamer tap -> FrameEncoder -> ProtocolConnection.send_frame
//        -> [ 127.0.0.1 TCP ] -> ProtocolConnection.recv_message -> FrameMessage -> FrameDecoder
//        -> pixels, which must equal the pixels we captured.
//
// It needs a device (the render + readback), so like the frame-tap proof it runs on lavapipe in CI
// and skips cleanly when there is none — unless RIME_REQUIRE_VULKAN is set, which makes "no device"
// a failure. This is the S0.6 loopback integration test (lavapipe frames); the *windowed* client
// and the live cross-machine view are S0.5/S0.6 on a machine with a display.
//
// It streams with LZ4 (lossless) on purpose: the decoded frame must be **bit-identical** to the one
// we captured, which is the strongest possible "the pipe carried it faithfully" assertion. (JPEG's
// lossy fidelity is already covered elsewhere.) doctest is not thread-safe, so the client runs on a
// thread that only records; the main thread renders, serves, and asserts after join().

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

#include "rime/platform/socket.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/protocol.hpp"

using namespace rime;

namespace {
bool require_vulkan() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

int u8(std::byte x) {
    return static_cast<int>(std::to_integer<std::uint8_t>(x));
}

// Clear `color` to a known colour (no pipeline/shaders needed), exactly as the frame-tap proof
// does.
void render_solid(rhi::Device& device, rhi::TextureHandle color, rhi::ClearColor clear) {
    auto cmd = device.begin_commands();
    rhi::RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = rhi::LoadOp::Clear;
    ri.color.store_op = rhi::StoreOp::Store;
    ri.color.clear = clear;
    cmd->begin_rendering(ri);
    cmd->end_rendering();
    device.submit_blocking(*cmd);
}
} // namespace

TEST_CASE("loopback: a rendered frame streams through the whole pipe and arrives bit-exact") {
    auto device = rhi::create_device({});
    if (!device) {
        if (require_vulkan()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the loopback stream proof");
        return;
    }

    // Render a known frame into a non-square target (catches stride/width-height mix-ups).
    const rhi::Extent2D extent{64, 48};
    rhi::TextureDesc td{};
    td.extent = extent;
    td.format = rhi::Format::RGBA8Unorm;
    td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    const rhi::TextureHandle color = device->create_texture(td);
    REQUIRE(color.is_valid());

    auto streamer = stream::FrameStreamer::create(*device, extent);
    REQUIRE(streamer.has_value());

    render_solid(*device, color, {0.25f, 0.55f, 0.85f, 1.0f});
    const stream::FrameView view = streamer->capture(color);
    REQUIRE(view.pixels.size() == 64u * 48u * 4u);

    // Copy the captured pixels out — this is the ground truth the far end must reproduce exactly.
    const std::vector<std::byte> captured(view.pixels.begin(), view.pixels.end());
    const stream::ImageDesc desc{extent, view.format};

    // Bring up a loopback endpoint and a client thread that receives + decodes the frame.
    auto listener = platform::TcpListener::bind(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->local_port();
    REQUIRE(port != 0);

    struct ClientOutcome {
        bool handshook = false;
        bool got_frame = false;
        bool decoded = false;
        std::uint32_t width = 0, height = 0;
        std::vector<std::byte> pixels;
    } out;

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
        stream::MessageType type{};
        std::vector<std::byte> payload;
        if (!conn.recv_message(type, payload) || type != stream::MessageType::Frame) {
            return;
        }
        out.got_frame = true;
        stream::FrameMessage fm;
        if (!fm.decode(payload)) {
            return;
        }
        out.width = fm.desc.extent.width;
        out.height = fm.desc.extent.height;
        std::vector<std::byte> pixels(fm.desc.byte_size());
        stream::FrameDecoder dec;
        if (!dec.decode(fm.codec, fm.desc, fm.data, pixels)) {
            return;
        }
        out.decoded = true;
        out.pixels = std::move(pixels);
    });

    // Server side (main thread): accept, handshake, encode the captured frame with LZ4, stream it.
    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    stream::ProtocolConnection server(std::move(*accepted));
    REQUIRE(server.handshake());

    stream::FrameEncoder enc;
    stream::FrameMessage fm;
    fm.sequence = 1;
    fm.codec = stream::Codec::LZ4; // lossless: the far end must match `captured` bit-for-bit
    fm.desc = desc;
    REQUIRE(enc.encode(stream::Codec::LZ4, desc, captured, fm.data));
    REQUIRE(server.send_frame(fm));

    client.join();

    CHECK(out.handshook);
    CHECK(out.got_frame);
    CHECK(out.decoded);
    CHECK(out.width == 64);
    CHECK(out.height == 48);
    // The whole point: what arrived equals what we captured, byte for byte.
    CHECK(out.pixels == captured);
    // And it is the colour we rendered (so the render really happened — not an all-zero frame).
    const std::size_t p = (10u * 64u + 10u) * 4u;
    CHECK(std::abs(u8(out.pixels[p + 0]) - 64) < 3);  // 0.25 * 255
    CHECK(std::abs(u8(out.pixels[p + 1]) - 140) < 3); // 0.55 * 255
    CHECK(std::abs(u8(out.pixels[p + 2]) - 217) < 3); // 0.85 * 255

    device->destroy(color);
}
