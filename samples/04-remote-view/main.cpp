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
//   client --window (S0.7): the *interactive* windowed viewer — present each decoded frame as a
//     full-window textured quad through a swapchain and forward real window key/mouse/scroll events
//     as InputEvents. Needs a display (a real screen, or Xvfb on a headless box).
//     run_client_windowed().
//
// The v0 scene is deliberately trivial: a full-screen **clear whose colour the remote input
// steers** (drag = R,G; scroll = B; any key = reset). No pipeline, no shaders — the sample is about
// the *stream and the control loop*, not the renderer; drop a real scene in later. "Keep v0 dumb."
//
// Run it (two terminals, same box or across a network):
//   remote_view server --port 9000 --codec jpeg                 (headless; waits for a client)
//   remote_view client --host 127.0.0.1 --port 9000 --frames 60 (headless client, writes PPMs)
//   remote_view client --window --host 127.0.0.1 --port 9000    (windowed viewer; needs a display)
//   xvfb-run remote_view client --window --selftest --frames 30 (windowed smoke, no server/display)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "rime/platform/platform.hpp" // window + event pump + input (the --window client)
#include "rime/platform/socket.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/latency.hpp"
#include "rime/stream/protocol.hpp"

// Build-time-compiled SPIR-V for the present quad (rime_add_shaders → quad_{vert,frag}_spv).
#include "quad.frag.spv.h"
#include "quad.vert.spv.h"

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
        case stream::Codec::Av1:
            // Av1 is the s1.2 inter-frame wire codec, but it is *stateful* — it runs through the
            // VideoEncoder/VideoDecoder pair + the StreamConfig/KeyframeRequest handshake, not this
            // sample's stateless FrameEncoder path. Switching this sample's server/client to the
            // video pipe by default is its own brick (tracked for the S1 close-out); until then the
            // `--codec` parser above never yields Av1, so this case only keeps the switch total.
            return "av1";
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
    // s1.3 latency ledger: the input thread records the identity + client-clock send time of the
    // most recently applied input; the frame loop echoes them so the client can close the
    // offset-free input-to-photon measurement (latency.hpp, ADR-0030 §5).
    std::atomic<std::uint32_t> last_input_seq{0};
    std::atomic<std::uint64_t> last_input_client_us{0};

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
                    last_input_seq.store(e.seq);
                    last_input_client_us.store(e.client_us);
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
        // Render the scene and submit its readback WITHOUT blocking (s1.1, ADR-0030): the GPU does
        // the copy while this thread moves on. try_get_frame() hands back the freshest *completed*
        // frame — latest-wins, so if encode/transport ever falls behind, stale frames are dropped
        // rather than queued. In steady state at 30 fps the copy is long finished by the next tick,
        // so frames flow 1:1 with a one-frame pipeline latency.
        render_scene(*device, color, scene.r.load(), scene.g.load(), scene.b.load());
        const std::uint64_t t_capture = now_us(); // ledger stage 1: the tap begins
        streamer->begin_capture(color);
        const std::optional<stream::FrameView> view = streamer->try_get_frame();
        if (view) {
            stream::FrameMessage fm;
            fm.sequence = seq;
            fm.capture_us = t_capture;
            fm.readback_us = now_us(); // stage 2: pixels in hand
            fm.last_input_seq = last_input_seq.load();
            fm.last_input_client_us = last_input_client_us.load();
            fm.codec = codec;
            fm.desc = {extent, view->format};
            if (!encoder.encode(codec, fm.desc, view->pixels, fm.data)) {
                std::fprintf(stderr, "remote_view server: encode failed\n");
                break;
            }
            fm.encode_us = now_us(); // stage 3: packet ready
            fm.wire_us = now_us();   // stage 4: just before send
            if (!conn.send_frame(fm)) {
                std::printf("remote_view server: client disconnected after %llu frames.\n",
                            static_cast<unsigned long long>(seq));
                break;
            }
            ++seq;
        }
        next += period;
        std::this_thread::sleep_until(next);
    }

    stop.store(true);
    input_thread.join();
    // Drain the tap before freeing the texture it captured from: the last begin_capture() may still
    // have a copy of `color` in flight, and destroying an image an unfinished command buffer
    // references is a use-after-free the validation layer rightly rejects. Resetting the streamer
    // waits those copies out (its destructor drains every pending ticket).
    const std::uint64_t dropped = streamer->stats().dropped;
    streamer.reset();
    device->destroy(color);
    std::printf("remote_view server: done (streamed %llu frames, %llu dropped latest-wins).\n",
                static_cast<unsigned long long>(seq),
                static_cast<unsigned long long>(dropped));
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
        std::uint32_t iseq = 0;
        for (int i = 0; i <= 100 && !stop.load(); ++i) {
            stream::InputEvent e;
            e.kind = stream::InputEvent::Kind::PointerMove;
            e.x = (i * 255) / 100; // 0 → 255 sweep drives R on the server
            e.y = 128;
            e.client_us = now_us(); // s1.3: stamp the send time + a per-client seq (the ledger echo)
            e.seq = ++iseq;
            if (!conn.send_input(e)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream::FrameDecoder decoder;
    stream::LatencyStats latency; // s1.3: per-stage ledger accumulated over the received frames
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
        const std::uint64_t t_recv = now_us(); // ledger stage 5: frame in hand
        stream::FrameMessage fm;
        if (!fm.decode(payload)) {
            break;
        }
        std::vector<std::byte> pixels(fm.desc.byte_size());
        if (!decoder.decode(fm.codec, fm.desc, fm.data, pixels)) {
            break;
        }
        const std::uint64_t t_decode = now_us(); // stage 6: pixels decoded
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

        // Fold this frame into the ledger: the server's stamps ride in `fm`; recv/decode/present are
        // ours; the echoed input (fm.last_input_*) closes the offset-free input-to-photon. A headless
        // client has no display, so "present" is the moment it finishes consuming the frame.
        stream::LatencyLedger ledger;
        ledger.capture_us = fm.capture_us;
        ledger.readback_us = fm.readback_us;
        ledger.encode_us = fm.encode_us;
        ledger.wire_us = fm.wire_us;
        ledger.recv_us = t_recv;
        ledger.decode_us = t_decode;
        ledger.present_us = now_us();
        ledger.input_seq = fm.last_input_seq;
        ledger.input_client_us = fm.last_input_client_us;
        latency.record(ledger);
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

    // s1.3: the latency ledger — median/p95 per stage over the session (ADR-0030 §5). On loopback
    // the numbers are tiny (same box, no real wire) but every stage is populated, proving the ledger
    // flows end to end; a real WAN client is where input->photon earns its keep.
    if (received > 0) {
        std::printf("%s", latency.dump().c_str());
    }

    if (!ppm_prefix.empty() && received > 0) {
        write_ppm(ppm_prefix + "-first.ppm", first_px, w, h);
        write_ppm(ppm_prefix + "-last.ppm", last_px, w, h);
        std::printf("remote_view client: wrote %s-first.ppm and %s-last.ppm\n",
                    ppm_prefix.c_str(),
                    ppm_prefix.c_str());
    }
    return received > 0 ? 0 : 1;
}

// ── The windowed client (S0.7) ─────────────────────────────────────────────────────────────────
// The interactive viewer: present each decoded frame as a full-window textured quad through a
// swapchain, and forward the window's key/mouse events back as stream::InputEvents. It reuses the
// 02-textured-quad present path (indexed quad + a combined image-sampler, the same quad.{vert,frag}
// shaders) with two changes for video — the quad fills the whole window, and its sampled texture is
// *dynamic*: re-uploaded from the freshly decoded RGBA every frame and re-created if the stream's
// size changes. Needs a display (a real screen, or Xvfb on a headless box).

// The presenter: a full-window quad whose texture we refresh each frame.
struct VideoQuad {
    rhi::Device& device;
    rhi::BufferHandle vbuf{}, ibuf{};
    rhi::SamplerHandle sampler{};
    rhi::ShaderHandle vsh{}, fsh{};
    rhi::PipelineHandle pipeline{};
    rhi::TextureHandle texture{};
    std::uint32_t tex_w = 0, tex_h = 0;

    VideoQuad(rhi::Device& d, rhi::Format color_format) : device(d) {
        using namespace rime::rhi;

        struct Vertex {
            float x, y, u, v;
        };

        // Fills the window (NDC -1..1). uv (0,0) sits at the top-left: Vulkan NDC y points down and
        // the decoded frame is top-row-first, so v=0 must land at the top of the screen (upright).
        static const Vertex vertices[] = {
            {-1.0f, -1.0f, 0.0f, 0.0f},
            {1.0f, -1.0f, 1.0f, 0.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
            {-1.0f, 1.0f, 0.0f, 1.0f},
        };
        static const std::uint16_t indices[] = {0, 1, 2, 2, 3, 0};

        BufferDesc vbd{};
        vbd.size = sizeof(vertices);
        vbd.usage = BufferUsage::Vertex;
        vbd.memory = MemoryUsage::CpuToGpu;
        vbd.initial_data = vertices;
        vbd.debug_name = "rv-quad-vertices";
        vbuf = device.create_buffer(vbd);

        BufferDesc ibd{};
        ibd.size = sizeof(indices);
        ibd.usage = BufferUsage::Index;
        ibd.memory = MemoryUsage::CpuToGpu;
        ibd.initial_data = indices;
        ibd.debug_name = "rv-quad-indices";
        ibuf = device.create_buffer(ibd);

        SamplerDesc smd{};
        smd.mag_filter = Filter::Linear; // smooth scaling of the frame to the window
        smd.min_filter = Filter::Linear;
        smd.address_mode = AddressMode::ClampToEdge;
        smd.debug_name = "rv-quad-sampler";
        sampler = device.create_sampler(smd);

        ShaderDesc vsd{};
        vsd.stage = ShaderStage::Vertex;
        vsd.spirv = quad_vert_spv;
        vsd.spirv_size_bytes = sizeof(quad_vert_spv);
        vsd.debug_name = "rv-quad.vert";
        vsh = device.create_shader(vsd);

        ShaderDesc fsd{};
        fsd.stage = ShaderStage::Fragment;
        fsd.spirv = quad_frag_spv;
        fsd.spirv_size_bytes = sizeof(quad_frag_spv);
        fsd.debug_name = "rv-quad.frag";
        fsh = device.create_shader(fsd);

        static const VertexAttribute attrs[] = {
            {0, Format::RG32Float, 0},
            {1, Format::RG32Float, sizeof(float) * 2},
        };
        GraphicsPipelineDesc pd{};
        pd.vertex_shader = vsh;
        pd.fragment_shader = fsh;
        pd.vertex_layout.stride = sizeof(Vertex);
        pd.vertex_layout.attributes = attrs;
        pd.color_format = color_format;
        pd.topology = PrimitiveTopology::TriangleList;
        pd.cull = CullMode::None;
        pd.sampled_texture = true;
        pd.debug_name = "rv-quad-pipeline";
        pipeline = device.create_graphics_pipeline(pd);
    }

    ~VideoQuad() {
        if (texture.is_valid()) {
            device.destroy(texture);
        }
        device.destroy(pipeline);
        device.destroy(fsh);
        device.destroy(vsh);
        device.destroy(sampler);
        device.destroy(ibuf);
        device.destroy(vbuf);
    }

    VideoQuad(const VideoQuad&) = delete;
    VideoQuad& operator=(const VideoQuad&) = delete;

    // (Re)create the sampled texture when the frame size changes, then upload the decoded RGBA.
    // This is the plain per-frame write_texture path; killing the upload stall with async readback
    // is s1.1's job — don't gold-plate it here.
    void upload(std::uint32_t w, std::uint32_t h, const void* rgba, std::size_t bytes) {
        if (w != tex_w || h != tex_h || !texture.is_valid()) {
            device.wait_idle(); // the old texture may still be read by a frame in flight
            if (texture.is_valid()) {
                device.destroy(texture);
            }
            rhi::TextureDesc td{};
            td.extent = {w, h};
            td.format = rhi::Format::RGBA8Unorm;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
            td.debug_name = "rv-video-texture";
            texture = device.create_texture(td);
            tex_w = w;
            tex_h = h;
        }
        device.write_texture(texture, rgba, bytes);
    }

    void record(rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D extent) {
        using namespace rime::rhi;
        RenderingInfo ri{};
        ri.color.target = target;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = {0.0f, 0.0f, 0.0f, 1.0f}; // letterbox any aspect mismatch in black
        cmd.begin_rendering(ri);
        cmd.bind_pipeline(pipeline);
        cmd.bind_texture(0, texture, sampler);
        cmd.bind_vertex_buffer(vbuf, 0);
        cmd.bind_index_buffer(ibuf, IndexType::Uint16, 0);
        Viewport vp{};
        vp.width = static_cast<float>(extent.width);
        vp.height = static_cast<float>(extent.height);
        vp.max_depth = 1.0f;
        cmd.set_viewport(vp);
        Rect2D scissor{};
        scissor.width = extent.width;
        scissor.height = extent.height;
        cmd.set_scissor(scissor);
        cmd.draw_indexed(6);
        cmd.end_rendering();
    }
};

// The recv thread decodes frames into here; the present thread takes the newest. A one-slot mailbox
// (not a queue): a slow presenter drops stale frames rather than falling behind — the right call
// for a live view where latest-wins beats every-frame.
struct FrameMailbox {
    std::mutex m;
    std::vector<std::byte> pixels;
    std::uint32_t w = 0, h = 0;
    bool dirty = false;

    void put(std::vector<std::byte>&& px, std::uint32_t width, std::uint32_t height) {
        std::lock_guard<std::mutex> lock(m);
        pixels = std::move(px);
        w = width;
        h = height;
        dirty = true;
    }

    // Move the newest frame out into `out` if one arrived since the last take; false otherwise.
    bool take(std::vector<std::byte>& out, std::uint32_t& width, std::uint32_t& height) {
        std::lock_guard<std::mutex> lock(m);
        if (!dirty) {
            return false;
        }
        out = std::move(pixels);
        width = w;
        height = h;
        dirty = false;
        return true;
    }
};

// A moving gradient for --selftest: successive frames differ (proving the per-frame texture upload
// + present actually cycles), no server required.
void synthesize_frame(std::vector<std::byte>& buf, std::uint32_t w, std::uint32_t h, int frame) {
    buf.resize(static_cast<std::size_t>(w) * h * 4);
    const int shift = (frame * 4) & 0xff;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::byte* p = &buf[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<std::byte>((x * 255 / w + shift) & 0xff);
            p[1] = static_cast<std::byte>(y * 255 / h);
            p[2] = static_cast<std::byte>(shift);
            p[3] = static_cast<std::byte>(255);
        }
    }
}

// Map a platform window event to a stream::InputEvent and send it. Pointer positions are scaled
// from window framebuffer pixels into the *frame's* pixel space (what "client pixels" means to the
// server: the coordinate system of the image it is sending), so a drag across the whole window
// sweeps the server's full input range regardless of window size. Escape asks the window to close.
void forward_input(stream::ProtocolConnection& conn,
                   const platform::Event& e,
                   platform::Extent2D win,
                   std::uint32_t frame_w,
                   std::uint32_t frame_h,
                   platform::Window& window) {
    using ET = platform::EventType;
    const auto to_frame = [](float pos, std::uint32_t win_dim, std::uint32_t frame_dim) {
        return win_dim == 0 ? 0
                            : static_cast<std::int32_t>(pos / static_cast<float>(win_dim) *
                                                        static_cast<float>(frame_dim));
    };
    stream::InputEvent ie;
    switch (e.type) {
        case ET::MouseMove:
            ie.kind = stream::InputEvent::Kind::PointerMove;
            ie.x = to_frame(e.mouse_move.x, win.width, frame_w);
            ie.y = to_frame(e.mouse_move.y, win.height, frame_h);
            break;
        case ET::MouseButton:
            ie.kind = e.button.down ? stream::InputEvent::Kind::PointerDown
                                    : stream::InputEvent::Kind::PointerUp;
            ie.code = static_cast<std::uint32_t>(e.button.button);
            break;
        case ET::MouseWheel:
            ie.kind = stream::InputEvent::Kind::PointerScroll;
            ie.scroll_x = e.wheel.dx;
            ie.scroll_y = e.wheel.dy;
            break;
        case ET::KeyDown:
            ie.kind = stream::InputEvent::Kind::KeyDown;
            ie.code = static_cast<std::uint32_t>(e.key.key);
            if (e.key.key == platform::Key::Escape) {
                window.request_close();
            }
            break;
        case ET::KeyUp:
            ie.kind = stream::InputEvent::Kind::KeyUp;
            ie.code = static_cast<std::uint32_t>(e.key.key);
            break;
        default:
            return; // not an event we forward
    }
    (void)conn.send_input(ie);
}

// RAII for the platform lifetime: init() in the ctor, shutdown() in the dtor. Declaring it FIRST in
// run_client_windowed means it is destroyed LAST — after the window — so the X11/Wayland connection
// outlives every Window (the teardown-order the window backends require; see window_x11.cpp).
struct PlatformSession {
    bool ok = rime::platform::init();

    ~PlatformSession() {
        if (ok) {
            rime::platform::shutdown();
        }
    }

    PlatformSession() = default;
    PlatformSession(const PlatformSession&) = delete;
    PlatformSession& operator=(const PlatformSession&) = delete;
};

int run_client_windowed(const std::string& host,
                        std::uint16_t port,
                        int max_frames,
                        bool selftest) {
    using namespace rime::platform;

    PlatformSession session; // first local ⇒ shutdown() runs last, after the window is gone
    if (!session.ok) {
        std::fprintf(stderr, "remote_view client --window: platform::init() failed\n");
        return 1;
    }

    WindowDesc wd{};
    wd.title = "Rime — remote view";
    wd.width = 1280;
    wd.height = 720;
    auto window = create_window(wd);
    if (!window) {
        // No display (a headless box/CI with no screen and no Xvfb). Treat it as a guarded SKIP,
        // the same way the samples skip when there is no Vulkan device — the windowed path is
        // proven where a screen exists (Xvfb here; a real display on the Mac).
        std::fprintf(
            stderr,
            "remote_view client --window: no display — skipping (run under a screen/Xvfb).\n");
        return 0;
    }
    window->show();

    rhi::DeviceDesc dd{};
    dd.app_name = "04-remote-view (client)";
    auto device = rhi::create_device(dd);
    if (!device) {
        std::fprintf(stderr, "remote_view client --window: no Vulkan device — skipping.\n");
        return std::getenv("RIME_REQUIRE_VULKAN") != nullptr ? 1 : 0;
    }

    rhi::SwapchainDesc sc_desc{};
    sc_desc.window = window->native_handle();
    const Extent2D fb = window->framebuffer_size();
    sc_desc.extent = {fb.width, fb.height};
    auto swapchain = device->create_swapchain(sc_desc);
    if (!swapchain) {
        std::fprintf(stderr, "remote_view client --window: could not create a swapchain\n");
        return 1;
    }

    std::optional<VideoQuad> quad;
    quad.emplace(*device, swapchain->format());

    // Connect to the server (unless self-testing): the recv thread decodes frames into the mailbox;
    // this (main) thread presents the newest and forwards input. One sender (main: send_input) +
    // one receiver (recv thread: recv_message) on the socket is the concurrency ProtocolConnection
    // allows.
    std::optional<stream::ProtocolConnection> conn;
    std::thread recv_thread;
    FrameMailbox mailbox;
    std::atomic<bool> stop{false};
    std::uint32_t frame_w = 256, frame_h = 256; // last known stream size (updates from real frames)

    if (!selftest) {
        auto sock = platform::TcpSocket::connect(host, port);
        if (!sock) {
            std::fprintf(stderr,
                         "remote_view client --window: could not connect to %s:%u\n",
                         host.c_str(),
                         port);
            return 1;
        }
        conn.emplace(std::move(*sock));
        if (!conn->handshake()) {
            std::fprintf(stderr, "remote_view client --window: handshake failed\n");
            return 1;
        }
        std::printf("remote_view client --window: connected to %s:%u — presenting on '%s'.\n",
                    host.c_str(),
                    port,
                    device->adapter().name.c_str());
        recv_thread = std::thread([&] {
            stream::FrameDecoder decoder;
            stream::MessageType type{};
            std::vector<std::byte> payload;
            while (!stop.load()) {
                if (!conn->recv_message(type, payload) || type == stream::MessageType::Bye) {
                    break;
                }
                if (type != stream::MessageType::Frame) {
                    continue;
                }
                stream::FrameMessage fm;
                if (!fm.decode(payload)) {
                    break;
                }
                std::vector<std::byte> pixels(fm.desc.byte_size());
                if (!decoder.decode(fm.codec, fm.desc, fm.data, pixels)) {
                    break;
                }
                mailbox.put(std::move(pixels), fm.desc.extent.width, fm.desc.extent.height);
            }
            stop.store(true); // the server went away ⇒ end the present loop too
        });
    } else {
        std::printf(
            "remote_view client --window: self-test (synthetic frames, no server) on '%s'.\n",
            device->adapter().name.c_str());
    }

    // Present loop: pump + forward input, refresh the texture from the newest frame, draw, present.
    std::vector<std::byte> pixels;
    std::uint32_t pw = 0, ph = 0;
    int presented = 0;
    while (pump_events() && !window->should_close() && !stop.load()) {
        if (max_frames > 0 && presented >= max_frames) {
            break;
        }

        Event e{};
        while (poll_event(e)) {
            if (e.type == EventType::WindowClose) {
                window->request_close();
            }
            if (conn) {
                forward_input(*conn, e, window->framebuffer_size(), frame_w, frame_h, *window);
            }
        }

        bool have_frame = false;
        if (selftest) {
            synthesize_frame(pixels, frame_w, frame_h, presented);
            pw = frame_w;
            ph = frame_h;
            have_frame = true;
        } else if (mailbox.take(pixels, pw, ph)) {
            frame_w = pw;
            frame_h = ph;
            have_frame = true;
        }
        if (!have_frame) {
            // Nothing new to show yet (waiting on the next server frame) — don't busy-spin.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        quad->upload(pw, ph, pixels.data(), pixels.size());
        rhi::TextureHandle target = swapchain->acquire_next_image();
        if (!target.is_valid()) {
            const Extent2D s = window->framebuffer_size();
            swapchain->recreate({s.width, s.height});
            continue;
        }
        auto cmd = device->begin_commands();
        quad->record(*cmd, target, swapchain->extent());
        if (!swapchain->present(*cmd)) {
            const Extent2D s = window->framebuffer_size();
            swapchain->recreate({s.width, s.height});
        }
        ++presented;
    }

    // Teardown: stop + say goodbye + join the recv thread BEFORE its std::thread destructs (a
    // joinable thread's destructor calls std::terminate), then idle the GPU so the present
    // resources (quad, swapchain) free cleanly as they go out of scope. The window is destroyed
    // before `session` (declared first), which is what keeps the display alive across the window's
    // destructor.
    stop.store(true);
    if (conn) {
        (void)conn->send_bye();
    }
    if (recv_thread.joinable()) {
        recv_thread.join();
    }
    device->wait_idle();
    std::printf("remote_view client --window: presented %d frame%s.%s\n",
                presented,
                presented == 1 ? "" : "s",
                selftest ? " (self-test)" : "");
    return presented > 0 ? 0 : 1;
}

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  remote_view server [--host H] [--port N] [--codec jpeg|lz4|raw] [--size N]\n"
                 "  remote_view client [--host H] [--port N] [--frames N] [--ppm PREFIX]\n"
                 "  remote_view client --window [--host H] [--port N] [--frames N]\n"
                 "        interactive viewer (needs a display); --frames N auto-exits after N\n"
                 "  remote_view client --window --selftest [--frames N]\n"
                 "        present synthetic frames with no server (the windowed smoke test)\n");
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
            // --frames 0 (the default) runs until the window is closed; a positive count auto-exits
            // (the smoke-test path). --selftest presents synthetic frames without a server.
            const int win_frames = std::atoi(flag(argc, argv, "--frames", "0").c_str());
            const bool selftest = has_flag(argc, argv, "--selftest");
            return run_client_windowed(host, port, win_frames < 0 ? 0 : win_frames, selftest);
        }
        const int frames = std::atoi(flag(argc, argv, "--frames", "60").c_str());
        return run_client_headless(
            host, port, frames <= 0 ? 60 : frames, flag(argc, argv, "--ppm", ""));
    }
    usage();
    return 2;
}
