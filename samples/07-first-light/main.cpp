// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 07-first-light — Milestone 5's "done when": a lit PBR scene drawn through the render graph,
// driven by the application framework. A metallic×roughness sphere grid on a mipmapped-checker
// floor, one ORBITING point light plus a little ambient, viewed with an orbit camera. It ties the
// whole milestone together: the app's fixed-tick loop (M5.7) advances a sim system that orbits the
// light; each frame the render callback draws the world with the SceneRenderer (M5.6) into the
// app-owned render graph (M5.4); the result is shown one of three ways —
//
//   --headless   render N frames off-screen, self-check that the scene is lit, exit 0/1
//   (CI/lavapipe)
//   --serve      stream it live over Track S0 (FrameStreamer → FrameEncoder → ProtocolConnection)
//   to
//                the 04-remote-view client, which views it and steers the camera (the dev-server →
//                Mac path); reuses the S0.5 server loop
//   (windowed)   present in a real window — the ADR-0023 §4 seam, needs a display (Mac); STUB here
//
// Run it:   build/dev/bin/first_light --headless [--frames 30] [--ppm out.ppm]
//           build/dev/bin/first_light --serve [--host 0.0.0.0] [--port 9100] [--codec jpeg]

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/system.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/clock.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/protocol.hpp"

namespace {

using namespace rime;

constexpr std::uint32_t kWidth = 1280;
constexpr std::uint32_t kHeight = 720; // 720p — the dev-server → Mac streaming budget

// ── The orbiting light: a sim component + system ─────────────────────────────────────────────────
// The light's motion is SIMULATION, advanced by the fixed tick — not by frame time — so it moves
// deterministically regardless of frame rate (M5.7). Its phase lives in a component; the system
// steps it and writes the light entity's world position onto a circle.
struct LightOrbit {
    float phase = 0.0f;
    float radius = 5.5f;
    float height = 4.5f;
    float speed = 0.8f; // radians per second
};

// The camera as orbit parameters, atomic so the --serve input thread can steer it while the frame
// loop reads it (independent scalars — the 04-remote-view pattern).
struct OrbitView {
    std::atomic<float> yaw{0.7f};
    std::atomic<float> pitch{0.32f};
    std::atomic<float> distance{12.0f};
};

// The camera entity's world transform from orbit parameters. The camera looks down its local −z
// (the convention SceneRenderer assumes), so its rotation is yaw about world-up then pitch about
// local-right, and the eye sits `distance` back along that view ray from `target`.
core::Transform camera_transform(float yaw, float pitch, float distance, core::Vec3 target) {
    // −pitch about local-right so a POSITIVE pitch raises the eye and tilts the view DOWN (the
    // intuitive sense), putting the camera above the grid looking down onto the floor.
    const core::Quat q = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) *
                                         core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -pitch));
    const core::Vec3 back =
        core::rotate(q, {0.0f, 0.0f, 1.0f}); // local +z (behind the view) in world
    core::Transform t{};
    t.rotation = q;
    t.translation = target + back * distance;
    return t;
}

// A mipmapped red/white checker for the floor: built on the CPU, uploaded with a full mip chain
// (write_texture blits it down — M5.3), sampled trilinear + anisotropic by the material sampler.
// sRGB format so the shader's sample decodes to linear. The mip/aniso path, underfoot.
rhi::TextureHandle make_checker(rhi::Device& device) {
    constexpr std::uint32_t kSize = 256;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(kSize) * kSize * 4);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool a = ((x / 32) + (y / 32)) % 2 == 0;
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * kSize + x) * 4];
            p[0] = a ? 205 : 35;
            p[1] = a ? 65 : 40;
            p[2] = a ? 55 : 48;
            p[3] = 255;
        }
    }
    rhi::TextureDesc td{};
    td.extent = {kSize, kSize};
    td.format = rhi::Format::RGBA8Srgb;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.mip_levels = 9; // log2(256)+1 — write_texture generates the chain via blits (M5.3)
    td.debug_name = "floor-checker";
    const rhi::TextureHandle t = device.create_texture(td);
    device.write_texture(t, px.data(), px.size());
    return t;
}

// Build the scene into the app's World: a 6×2 metallic(top)/dielectric(bottom) sphere grid of
// rising roughness over a big checkered floor, plus the orbiting light. Returns the light + camera
// entities so the loop can pose them, and registers the orbit-light sim system on the app's
// schedule.
void build_scene(app::Application& app,
                 render::MeshRegistry& meshes,
                 render::MaterialRegistry& materials,
                 rhi::TextureHandle checker,
                 ecs::Entity& out_camera) {
    using ecs::WorldTransform;
    ecs::World& world = app.world();
    render::register_render_components(world);
    (void)world.register_component<LightOrbit>();

    const render::MeshId sphere = meshes.add(render::make_uv_sphere(0.75f, 48, 96), "sphere");
    const render::MeshId floor = meshes.add(render::make_plane(14.0f, 8.0f), "floor"); // tiles 8×

    constexpr int kCols = 6;
    constexpr float kSpacing = 2.0f;
    for (int c = 0; c < kCols; ++c) {
        const float rough = 0.05f + 0.9f * static_cast<float>(c) / static_cast<float>(kCols - 1);
        const float x = (static_cast<float>(c) - (kCols - 1) * 0.5f) * kSpacing;

        render::PbrMaterialDesc metal{};
        metal.base_color[0] = 0.95f;
        metal.base_color[1] = 0.64f;
        metal.base_color[2] = 0.54f; // copper
        metal.metallic = 1.0f;
        metal.roughness = rough;
        core::Transform tf{};
        tf.translation = {x, 1.1f, 0.0f};
        (void)world.spawn_with(
            WorldTransform{tf}, render::MeshRef{sphere}, render::MaterialRef{materials.add(metal)});

        render::PbrMaterialDesc dielectric{};
        dielectric.base_color[0] = 0.10f;
        dielectric.base_color[1] = 0.32f;
        dielectric.base_color[2] = 0.75f; // blue plastic
        dielectric.metallic = 0.0f;
        dielectric.roughness = rough;
        tf.translation = {x, -1.1f, 0.0f};
        (void)world.spawn_with(WorldTransform{tf},
                               render::MeshRef{sphere},
                               render::MaterialRef{materials.add(dielectric)});
    }

    render::PbrMaterialDesc floor_mat{};
    floor_mat.metallic = 0.0f;
    floor_mat.roughness = 0.75f;
    floor_mat.base_color_texture = checker;
    core::Transform floor_tf{};
    floor_tf.translation = {0.0f, -2.1f, 0.0f};
    (void)world.spawn_with(WorldTransform{floor_tf},
                           render::MeshRef{floor},
                           render::MaterialRef{materials.add(floor_mat)});

    out_camera = world.spawn_with(
        WorldTransform{camera_transform(0.7f, 0.32f, 12.0f, {0.0f, 0.0f, 0.0f})}, render::Camera{});

    core::Transform lt{};
    lt.translation = {5.5f, 4.5f, 0.0f};
    (void)world.spawn_with(
        WorldTransform{lt}, render::PointLight{1.0f, 0.92f, 0.82f, 90.0f, 26.0f}, LightOrbit{});

    // The orbit-light system: advance the phase by the fixed dt and place the light on its circle.
    // It WRITES LightOrbit (the phase) and WorldTransform (the position); declared truthfully so
    // the scheduler's hazard analysis is sound (ADR-0018).
    const double dt = app.fixed_dt();
    app.schedule().add(
        "orbit-light",
        ecs::SystemAccess{{}, ecs::signature_of<LightOrbit, ecs::WorldTransform>(world)},
        [dt](ecs::World& w, core::JobSystem&, ecs::CommandBuffer&) {
            w.query<LightOrbit, WorldTransform>().for_each([dt](LightOrbit& o, WorldTransform& wt) {
                o.phase += static_cast<float>(dt) * o.speed;
                wt.value.translation = {
                    o.radius * std::cos(o.phase), o.height, o.radius * std::sin(o.phase)};
            });
        });
}

// ── Small I/O helpers (the 06/tests pattern) ─────────────────────────────────────────────────────
std::vector<std::uint8_t>
read_rgba8(rhi::Device& device, rhi::TextureHandle tex, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size = bytes;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle rb = device.create_buffer(bd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

bool scene_is_lit(const std::vector<std::uint8_t>& px) {
    std::uint64_t lit = 0, bright = 0;
    const std::size_t n = px.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int lum = (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3;
        if (lum > 25)
            ++lit;
        if (lum > 170)
            ++bright;
    }
    std::printf("  self-check: %llu lit / %llu bright of %zu px\n",
                static_cast<unsigned long long>(lit),
                static_cast<unsigned long long>(bright),
                n);
    return lit > n / 40 && bright > 30;
}

void write_ppm(const char* path,
               const std::vector<std::uint8_t>& px,
               std::uint32_t w,
               std::uint32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
        std::fwrite(&px[i * 4], 1, 3, f);
    std::fclose(f);
    std::printf("  wrote %s\n", path);
}

// A rendering app: the pieces every mode shares. Owns the GPU app, the registries, the checker, and
// the SceneRenderer, wired so a frame draws the world into the app's graph and stashes the output.
struct FirstLightApp {
    app::Application& app; // borrowed — the caller constructs it and CHECKS its device first
    render::MeshRegistry meshes;
    render::MaterialRegistry materials;
    rhi::TextureHandle checker{};
    render::SceneRenderer renderer;
    ecs::Entity camera{};
    render::RGTexture last_ldr{};

    // Precondition: `application.device() != nullptr`. The device check is deliberately the
    // CALLER's job, not ours: the GPU resources below dereference the device in this initializer
    // list, so a GPU-less host has to be handled BEFORE we get here — dereferencing a null device
    // is exactly what crashed the headless run on a runner with no Vulkan driver.
    explicit FirstLightApp(app::Application& application)
        : app(application), meshes(*app.device()), renderer(*app.device(), meshes, materials) {
        checker = make_checker(*app.device());
        build_scene(app, meshes, materials, checker, camera);
        renderer.set_ambient(0.03f, 0.03f, 0.04f);
        app.on_render([this](app::FrameContext& ctx) {
            last_ldr = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
        });
    }

    ~FirstLightApp() { app.device()->destroy(checker); }

    FirstLightApp(const FirstLightApp&) = delete;
    FirstLightApp& operator=(const FirstLightApp&) = delete;

    void pose_camera(float yaw, float pitch, float distance) {
        app.world().get<ecs::WorldTransform>(camera)->value =
            camera_transform(yaw, pitch, distance, {0.0f, -0.2f, 0.0f});
    }
};

app::AppConfig gpu_config() {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kWidth, kHeight};
    cfg.tick_hz = 60.0;
    return cfg;
}

// ── --headless: render, self-check, exit code ────────────────────────────────────────────────────
int run_headless(int frames, const char* ppm) {
    // Create + device-check the app BEFORE building any GPU resources on it: on a host with no
    // Vulkan device (a GPU-less CI runner) app.device() is null, and FirstLightApp's constructor
    // would dereference it. Absent GPU is a skip (exit 0) unless RIME_REQUIRE_VULKAN demands one.
    app::Application app(gpu_config());
    if (!app.device()) {
        std::fprintf(stderr, "07-first-light: no Vulkan device (need a driver or lavapipe)\n");
        return std::getenv("RIME_REQUIRE_VULKAN") ? 1 : 0; // absent GPU: a skip, unless required
    }
    FirstLightApp fl(app);
    std::printf("07-first-light: rendering %d frame(s) on '%s' (%ux%u)\n",
                frames,
                fl.app.device()->adapter().name.c_str(),
                kWidth,
                kHeight);
    fl.pose_camera(0.4f, 0.38f, 12.5f);

    for (int i = 0; i < frames; ++i)
        fl.app.step(fl.app.fixed_dt());

    const std::vector<std::uint8_t> px =
        read_rgba8(*fl.app.device(), fl.app.graph()->physical(fl.last_ldr), kWidth, kHeight);
    const bool ok = scene_is_lit(px);
    if (ppm)
        write_ppm(ppm, px, kWidth, kHeight);
    std::printf("07-first-light: %s (%llu ticks simulated)\n",
                ok ? "scene is lit — first light!" : "FAILED self-check",
                static_cast<unsigned long long>(fl.app.tick_count()));
    return ok ? 0 : 1;
}

// ── --serve: stream it live and let the client steer the camera (Track S0) ───────────────────────
std::uint64_t now_us() {
    return platform::Clock::now_ns() / 1000;
}

void apply_input(OrbitView& v, const stream::InputEvent& e) {
    using K = stream::InputEvent::Kind;
    switch (e.kind) {
        case K::PointerMove: // drag: x → yaw (full turn across the width), y → pitch
            v.yaw.store((static_cast<float>(e.x) / static_cast<float>(kWidth)) * 6.2831853f);
            v.pitch.store(
                std::clamp((0.5f - static_cast<float>(e.y) / static_cast<float>(kHeight)) * 2.4f,
                           -1.4f,
                           1.4f));
            break;
        case K::PointerScroll: // wheel: zoom
            v.distance.store(std::clamp(v.distance.load() - e.scroll_y * 0.8f, 4.0f, 30.0f));
            break;
        case K::KeyDown: // reset
            v.yaw.store(0.7f);
            v.pitch.store(0.32f);
            v.distance.store(12.0f);
            break;
        default:
            break;
    }
}

int run_serve(const std::string& host, std::uint16_t port, stream::Codec codec) {
    // Device-check before building GPU resources (see run_headless) — no device is a hard error for
    // the server, which exists to stream rendered frames.
    app::Application app(gpu_config());
    if (!app.device()) {
        std::fprintf(stderr, "07-first-light server: no Vulkan device (need lavapipe/a GPU)\n");
        return 1;
    }
    FirstLightApp fl(app);
    auto streamer = stream::FrameStreamer::create(*fl.app.device(), {kWidth, kHeight});
    if (!streamer) {
        std::fprintf(stderr, "07-first-light server: could not create the frame streamer\n");
        return 1;
    }
    auto listener = platform::TcpListener::bind(port, host);
    if (!listener) {
        std::fprintf(stderr, "07-first-light server: could not bind %s:%u\n", host.c_str(), port);
        return 1;
    }
    std::printf("07-first-light server: rendering on '%s'; listening on %s:%u — waiting for a "
                "client…\n",
                fl.app.device()->adapter().name.c_str(),
                host.c_str(),
                listener->local_port());
    auto accepted = listener->accept();
    if (!accepted) {
        std::fprintf(stderr, "07-first-light server: accept failed\n");
        return 1;
    }
    stream::ProtocolConnection conn(std::move(*accepted));
    if (!conn.handshake()) {
        std::fprintf(stderr, "07-first-light server: handshake failed\n");
        return 1;
    }
    std::printf("07-first-light server: client connected — streaming 720p.\n");

    OrbitView view;
    std::atomic<bool> stop{false};

    // Input thread: drain the client's InputEvents into the shared orbit view. One sender (the
    // frame loop) + one receiver (this) on the full-duplex socket — the concurrency
    // ProtocolConnection allows.
    std::thread input_thread([&] {
        stream::MessageType type{};
        std::vector<std::byte> payload;
        while (!stop.load()) {
            if (!conn.recv_message(type, payload) || type == stream::MessageType::Bye)
                break;
            if (type == stream::MessageType::Input) {
                stream::InputEvent e;
                if (e.decode(payload))
                    apply_input(view, e);
            }
        }
        stop.store(true);
    });

    // Frame loop: pose the camera from the (client-steered) orbit view, step the app (which orbits
    // the light and renders the frame into the graph), tap the rendered image, encode, send — paced
    // to ~30 fps — until the client goes away. Real elapsed dt drives the sim so the light orbits
    // at wall-clock rate.
    stream::FrameEncoder encoder;
    std::uint64_t seq = 0;
    std::uint64_t last_ns = platform::Clock::now_ns();
    const auto period = std::chrono::milliseconds(33);
    auto next = std::chrono::steady_clock::now();
    while (!stop.load()) {
        const std::uint64_t now_ns = platform::Clock::now_ns();
        const double dt = static_cast<double>(now_ns - last_ns) * 1e-9;
        last_ns = now_ns;

        fl.pose_camera(view.yaw.load(), view.pitch.load(), view.distance.load());
        fl.app.step(dt);

        const stream::FrameView fv = streamer->capture(fl.app.graph()->physical(fl.last_ldr));
        stream::FrameMessage fm;
        fm.sequence = seq;
        fm.capture_us = now_us();
        fm.codec = codec;
        fm.desc = {{kWidth, kHeight}, fv.format};
        if (!encoder.encode(codec, fm.desc, fv.pixels, fm.data)) {
            std::fprintf(stderr, "07-first-light server: encode failed\n");
            break;
        }
        if (!conn.send_frame(fm)) {
            std::printf("07-first-light server: client disconnected after %llu frames.\n",
                        static_cast<unsigned long long>(seq));
            break;
        }
        ++seq;
        next += period;
        std::this_thread::sleep_until(next);
    }

    stop.store(true);
    input_thread.join();
    std::printf("07-first-light server: done (streamed %llu frames, avg capture %.2f ms).\n",
                static_cast<unsigned long long>(seq),
                streamer->stats().avg_ms);
    return 0;
}

stream::Codec parse_codec(std::string_view s) {
    if (s == "raw")
        return stream::Codec::Raw;
    if (s == "lz4")
        return stream::Codec::LZ4;
    return stream::Codec::Jpeg; // the wire default (ADR-0017)
}

} // namespace

int main(int argc, char** argv) {
    enum class Mode { Headless, Serve, Windowed } mode = Mode::Headless;
    int frames = 30;
    const char* ppm = nullptr;
    std::string host = "0.0.0.0";
    std::uint16_t port = 9100;
    stream::Codec codec = stream::Codec::Jpeg;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--headless")
            mode = Mode::Headless;
        else if (a == "--serve")
            mode = Mode::Serve;
        else if (a == "--windowed")
            mode = Mode::Windowed;
        else if (a == "--frames" && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (a == "--ppm" && i + 1 < argc)
            ppm = argv[++i];
        else if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--codec" && i + 1 < argc)
            codec = parse_codec(argv[++i]);
    }

    if (mode == Mode::Windowed) {
        // Presenting to a swapchain needs a display; this box is headless (ADR-0023 §4). The
        // windowed path lands with the Mac build — until then, be honest and run headless.
        std::printf(
            "07-first-light: --windowed needs a display (Mac); running --headless instead.\n");
        mode = Mode::Headless;
    }
    if (mode == Mode::Serve)
        return run_serve(host, port, codec);
    return run_headless(frames, ppm);
}
