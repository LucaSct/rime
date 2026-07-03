// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 04-remote-view — the S0 remote-view endpoint: "see and control a headless Rime instance from
// elsewhere." It ties the whole Track-S stack together into a runnable program:
//
//   server (headless): render a scene off-screen -> FrameStreamer tap (S0.2) -> FrameEncoder (S0.3)
//     -> ProtocolConnection (S0.4) over TCP (S0.1) to a connected client, while applying the input
//     the client sends back. This is the endpoint you run on a headless box (e.g. a lavapipe
//     server).
//   client --headless: connect, script some input, receive+decode frames, and report — a GPU-free
//     client that proves the round-trip anywhere (and writes PPMs with --ppm).
//   client --window: the *interactive* windowed viewer — needs a display; see
//   run_client_windowed().
//
// The v0 scene is deliberately trivial: a full-screen **clear whose colour the remote input
// steers** (drag = R,G; scroll = B; any key = reset). No pipeline, no shaders — the sample is about
// the *stream and the control loop*, not the renderer; drop a real scene in later. "Keep v0 dumb."
//
// Run it (two terminals, same box or across a network):
//   remote_view server --port 9000 --codec jpeg                 (headless; waits for a client)
//   remote_view client --host 127.0.0.1 --port 9000 --frames 60

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rime/platform/socket.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/protocol.hpp"

using namespace rime;

namespace {

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

int u8(std::byte x) {
    return static_cast<int>(std::to_integer<std::uint8_t>(x));
}

stream::Codec parse_codec(const std::string& s) {
    if (s == "lz4") {
        return stream::Codec::LZ4;
    }
    if (s == "raw") {
        return stream::Codec::Raw;
    }
    return stream::Codec::Jpeg; // default: the wire codec
}

const char* codec_name(stream::Codec c) {
    switch (c) {
        case stream::Codec::Raw:
            return "raw";
        case stream::Codec::LZ4:
            return "lz4";
        case stream::Codec::Jpeg:
            return "jpeg";
    }
    return "?";
}

// ── The scene: a colour the remote input steers ──────────────────────────────────────────────────
// std::atomic scalars, so the input thread can update it while the render thread reads it with no
// lock (the values are independent). This stands in for "the game world the remote player
// controls".
struct SceneState {
    std::atomic<float> r{0.20f};
    std::atomic<float> g{0.30f};
    std::atomic<float> b{0.50f};
};

void apply_input(SceneState& scene, const stream::InputEvent& e) {
    using K = stream::InputEvent::Kind;
    switch (e.kind) {
        case K::PointerMove: // drag steers R (x) and G (y)
            scene.r.store(static_cast<float>(std::clamp(e.x, 0, 255)) / 255.0f);
            scene.g.store(static_cast<float>(std::clamp(e.y, 0, 255)) / 255.0f);
            break;
        case K::PointerScroll: // wheel nudges B
            scene.b.store(std::clamp(scene.b.load() + e.scroll_y * 0.05f, 0.0f, 1.0f));
            break;
        case K::KeyDown: // any key resets
            scene.r.store(0.20f);
            scene.g.store(0.30f);
            scene.b.store(0.50f);
            break;
        default:
            break;
    }
}

// Clear `color` to a solid colour — a whole "render" with no pipeline or shaders (the S0 scene).
void render_scene(rhi::Device& device, rhi::TextureHandle color, float r, float g, float bl) {
    auto cmd = device.begin_commands();
    rhi::RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = rhi::LoadOp::Clear;
    ri.color.store_op = rhi::StoreOp::Store;
    ri.color.clear = {r, g, bl, 1.0f};
    cmd->begin_rendering(ri);
    cmd->end_rendering();
    device.submit_blocking(*cmd);
}

// ── The server ───────────────────────────────────────────────────────────────────────────────────
int run_server(const std::string& host,
               std::uint16_t port,
               stream::Codec codec,
               std::uint32_t size) {
    rhi::DeviceDesc dd{};
    dd.app_name = "04-remote-view (server)";
    auto device = rhi::create_device(dd);
    if (!device) {
        std::fprintf(stderr, "remote_view server: no Vulkan device (need a driver or lavapipe)\n");
        return 1;
    }
    std::printf("remote_view server: rendering on '%s'\n", device->adapter().name.c_str());

    const rhi::Extent2D extent{size, size};
    rhi::TextureDesc td{};
    td.extent = extent;
    td.format = rhi::Format::RGBA8Unorm;
    td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    const rhi::TextureHandle color = device->create_texture(td);
    if (!color.is_valid()) {
        std::fprintf(stderr, "remote_view server: could not create the render target\n");
        return 1;
    }
    auto streamer = stream::FrameStreamer::create(*device, extent);
    if (!streamer) {
        std::fprintf(stderr, "remote_view server: could not create the frame streamer\n");
        return 1;
    }

    auto listener = platform::TcpListener::bind(port, host);
    if (!listener) {
        std::fprintf(stderr, "remote_view server: could not bind %s:%u\n", host.c_str(), port);
        return 1;
    }
    std::printf("remote_view server: listening on %s:%u (%s) — waiting for a client…\n",
                host.c_str(),
                listener->local_port(),
                codec_name(codec));
    auto accepted = listener->accept();
    if (!accepted) {
        std::fprintf(stderr, "remote_view server: accept failed\n");
        return 1;
    }
    stream::ProtocolConnection conn(std::move(*accepted));
    if (!conn.handshake()) {
        std::fprintf(stderr, "remote_view server: handshake failed\n");
        return 1;
    }
    std::printf("remote_view server: client connected — streaming.\n");

    SceneState scene;
    std::atomic<bool> stop{false};

    // Input thread: drain the client's InputEvents and apply them. When the client disconnects,
    // recv_message returns false (EOF), so this ends on its own and signals the frame loop to stop.
    // Safe alongside the frame loop's sends: one sender thread + one receiver thread on a
    // full-duplex socket is the concurrency ProtocolConnection allows (see its docs).
    std::thread input_thread([&] {
        stream::MessageType type{};
        std::vector<std::byte> payload;
        while (!stop.load()) {
            if (!conn.recv_message(type, payload)) {
                break;
            }
            if (type == stream::MessageType::Bye) {
                break;
            }
            if (type == stream::MessageType::Input) {
                stream::InputEvent e;
                if (e.decode(payload)) {
                    apply_input(scene, e);
                }
            }
        }
        stop.store(true);
    });

    // Frame loop: render the current scene, tap it, encode it, send it — ~30 fps — until the client
    // goes away (send fails) or the input thread signals stop. The client decides the session
    // length.
    stream::FrameEncoder encoder;
    std::uint64_t seq = 0;
    const auto period = std::chrono::milliseconds(33); // ~30 fps
    auto next = std::chrono::steady_clock::now();
    while (!stop.load()) {
        render_scene(*device, color, scene.r.load(), scene.g.load(), scene.b.load());
        const stream::FrameView view = streamer->capture(color);

        stream::FrameMessage fm;
        fm.sequence = seq;
        fm.capture_us = now_us();
        fm.codec = codec;
        fm.desc = {extent, view.format};
        if (!encoder.encode(codec, fm.desc, view.pixels, fm.data)) {
            std::fprintf(stderr, "remote_view server: encode failed\n");
            break;
        }
        if (!conn.send_frame(fm)) {
            std::printf("remote_view server: client disconnected after %llu frames.\n",
                        static_cast<unsigned long long>(seq));
            break;
        }
        ++seq;
        next += period;
        std::this_thread::sleep_until(next);
    }

    stop.store(true);
    input_thread.join();
    device->destroy(color);
    std::printf("remote_view server: done (streamed %llu frames, avg capture %.2f ms).\n",
                static_cast<unsigned long long>(seq),
                streamer->stats().avg_ms);
    return 0;
}

// ── The headless client ──────────────────────────────────────────────────────────────────────────
void write_ppm(const std::string& path,
               const std::vector<std::byte>& rgba,
               std::uint32_t w,
               std::uint32_t h) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(u8(rgba[i * 4 + 0])),
                                      static_cast<unsigned char>(u8(rgba[i * 4 + 1])),
                                      static_cast<unsigned char>(u8(rgba[i * 4 + 2]))};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int run_client_headless(const std::string& host,
                        std::uint16_t port,
                        int frames,
                        const std::string& ppm_prefix) {
    auto sock = platform::TcpSocket::connect(host, port);
    if (!sock) {
        std::fprintf(
            stderr, "remote_view client: could not connect to %s:%u\n", host.c_str(), port);
        return 1;
    }
    stream::ProtocolConnection conn(std::move(*sock));
    if (!conn.handshake()) {
        std::fprintf(stderr, "remote_view client: handshake failed (wrong version/protocol?)\n");
        return 1;
    }
    std::printf("remote_view client: connected to %s:%u — receiving %d frames.\n",
                host.c_str(),
                port,
                frames);

    // Sender thread: sweep a pointer drag across the frame so the server's colour visibly changes —
    // this is the "control" half proving the backchannel works end to end.
    std::atomic<bool> stop{false};
    std::thread sender([&] {
        for (int i = 0; i <= 100 && !stop.load(); ++i) {
            stream::InputEvent e;
            e.kind = stream::InputEvent::Kind::PointerMove;
            e.x = (i * 255) / 100; // 0 → 255 sweep drives R on the server
            e.y = 128;
            if (!conn.send_input(e)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream::FrameDecoder decoder;
    stream::MessageType type{};
    std::vector<std::byte> payload;
    int received = 0;
    std::uint64_t first_seq = 0, last_seq = 0;
    int first_r = -1, last_r = -1;
    std::uint32_t w = 0, h = 0;
    std::vector<std::byte> first_px, last_px;

    for (int i = 0; i < frames; ++i) {
        if (!conn.recv_message(type, payload) || type != stream::MessageType::Frame) {
            break;
        }
        stream::FrameMessage fm;
        if (!fm.decode(payload)) {
            break;
        }
        std::vector<std::byte> pixels(fm.desc.byte_size());
        if (!decoder.decode(fm.codec, fm.desc, fm.data, pixels)) {
            break;
        }
        w = fm.desc.extent.width;
        h = fm.desc.extent.height;
        const std::size_t center = (static_cast<std::size_t>(h / 2) * w + w / 2) * 4;
        const int cr = u8(pixels[center]);
        if (received == 0) {
            first_seq = fm.sequence;
            first_r = cr;
            first_px = pixels;
        }
        last_seq = fm.sequence;
        last_r = cr;
        last_px = std::move(pixels);
        ++received;
    }

    stop.store(true);
    (void)conn.send_bye(); // best-effort graceful close
    sender.join();

    std::printf(
        "remote_view client: got %d frames (seq %llu…%llu), %ux%u; centre R first=%d last=%d %s\n",
        received,
        static_cast<unsigned long long>(first_seq),
        static_cast<unsigned long long>(last_seq),
        w,
        h,
        first_r,
        last_r,
        (received > 1 && last_r != first_r) ? "— scene responded to input ✓" : "");

    if (!ppm_prefix.empty() && received > 0) {
        write_ppm(ppm_prefix + "-first.ppm", first_px, w, h);
        write_ppm(ppm_prefix + "-last.ppm", last_px, w, h);
        std::printf("remote_view client: wrote %s-first.ppm and %s-last.ppm\n",
                    ppm_prefix.c_str(),
                    ppm_prefix.c_str());
    }
    return received > 0 ? 0 : 1;
}

int run_client_windowed() {
    // The interactive windowed client needs a display, which this (possibly headless) build/host
    // may not have — so it is intentionally not implemented here yet. It is small, and squarely on
    // the roadmap (S0.5): present the decoded RGBA with the 02-textured-quad path (upload to a
    // texture, draw a full-screen quad through a swapchain), and translate platform window
    // key/mouse events into stream::InputEvent on the backchannel. Build and run it on a machine
    // with a screen (macOS/MoltenVK first). See docs/design/graphics-streaming.md.
    std::fprintf(
        stderr,
        "remote_view: the windowed client needs a display and isn't built yet (S0.5).\n"
        "Use `client --headless` here (GPU-free), or implement/run the windowed viewer on a\n"
        "machine with a screen — it reuses the 02-textured-quad present path plus a\n"
        "window-event → stream::InputEvent mapping. See docs/design/graphics-streaming.md.\n");
    return 2;
}

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  remote_view server [--host H] [--port N] [--codec jpeg|lz4|raw] [--size N]\n"
                 "  remote_view client [--host H] [--port N] [--frames N] [--ppm PREFIX]\n"
                 "  remote_view client --window   (interactive; needs a display — see the note)\n");
}

// Tiny flag lookup: returns the value after `name`, or `fallback` if absent.
std::string flag(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 0; i < argc - 1; ++i) {
        if (name == argv[i]) {
            return argv[i + 1];
        }
    }
    return fallback;
}

bool has_flag(int argc, char** argv, const std::string& name) {
    for (int i = 0; i < argc; ++i) {
        if (name == argv[i]) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string role = argv[1];
    const std::string host = flag(argc, argv, "--host", "127.0.0.1");
    const auto port =
        static_cast<std::uint16_t>(std::atoi(flag(argc, argv, "--port", "9000").c_str()));

    if (role == "server") {
        const stream::Codec codec = parse_codec(flag(argc, argv, "--codec", "jpeg"));
        const auto size =
            static_cast<std::uint32_t>(std::atoi(flag(argc, argv, "--size", "256").c_str()));
        return run_server(host, port, codec, size == 0 ? 256 : size);
    }
    if (role == "client") {
        if (has_flag(argc, argv, "--window")) {
            return run_client_windowed();
        }
        const int frames = std::atoi(flag(argc, argv, "--frames", "60").c_str());
        return run_client_headless(
            host, port, frames <= 0 ? 60 : frames, flag(argc, argv, "--ppm", ""));
    }
    usage();
    return 2;
}
