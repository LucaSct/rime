// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// `rime-engine --editor-host <socket> [--scene <file>] [--viewport]` — the engine as the editor's
// live host process (M9, ADR-0016/0031). The Rust editor (tools/editor) launches this and connects
// over the s1.4 local socket.
//
// Two modes share one connection:
//   * The **editor channel** (always): send the component schema + a full-world snapshot, then
//   drain
//     and apply the client's edits (editorhost). GPU-free.
//   * The **streamed viewport** (`--viewport`, needs a GPU/lavapipe): render a live scene through
//   the
//     render graph, capture each frame, LZ4-compress it, and send it as a Frame message — the
//     editor draws it in its viewport panel. Edits from the channel mutate the very world being
//     rendered, so a change shows up in the next streamed frame (the live edit→viewport loop).
//
// The connection is full-duplex: one thread sends (schema/snapshot/frames), one receives (edits).
// The World is touched only by the send/render thread — the receiver just queues raw edits, which
// the render thread applies at a frame boundary (the ECS structural-change rule, ADR-0018).

#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/core/byte_cursor.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/protocol.hpp"

namespace {

using namespace rime;

constexpr std::uint32_t kViewportWidth = 960;
constexpr std::uint32_t kViewportHeight = 540;

// ── Editor channel (GPU-free) ─────────────────────────────────────────────────────────────

// A small default scene so `rime-engine --editor-host <socket>` (no --scene) still hands the editor
// something to inspect. Mirrors the samples/07-first-light shape — the world the m9.2 fixture
// holds.
void build_default_world(ecs::World& world) {
    const auto place = [](float x, float y, float z) {
        return ecs::LocalTransform{core::Transform{
            core::Vec3{x, y, z}, core::quat_identity(), core::Vec3{1.0f, 1.0f, 1.0f}}};
    };
    (void)world.spawn_with(place(0.0f, 1.5f, 4.0f), render::Camera{0.9f, 0.1f, 500.0f, true});
    (void)world.spawn_with(place(0.0f, 10.0f, 0.0f),
                           render::DirectionalLight{1.0f, 0.95f, 0.9f, 3.0f});
    const ecs::Entity ground =
        world.spawn_with(place(0.0f, 0.0f, 0.0f), render::MeshRef{0}, render::MaterialRef{0});
    (void)world.spawn_with(
        place(1.0f, 0.0f, 0.0f), render::MeshRef{1}, render::MaterialRef{1}, ecs::Parent{ground});
}

void register_and_populate(ecs::World& world, std::string_view scene_path) {
    ecs::register_transform_components(world);
    render::register_render_components(world);
    if (scene_path.empty()) {
        build_default_world(world);
    } else {
        const scene::LoadReport report =
            scene::load_scene_file(world, std::filesystem::path(scene_path));
        if (report.ok) {
            core::JobSystem jobs;
            ecs::propagate_transforms(world, jobs);
        } else {
            RIME_ERROR("rime-engine: scene load failed ({}): {}", scene_path, report.error);
        }
    }
}

// Apply one editor->engine edit to `world`. Mirrors editorhost::EditorHost::poll_one's dispatch,
// but split out so the render thread can apply an edit the receiver thread parsed off the wire.
// Call at a frame boundary (single-threaded world access).
void apply_edit(ecs::World& world, stream::MessageType type, std::span<const std::byte> payload) {
    using editorhost::EditorMessage;
    switch (static_cast<EditorMessage>(type)) {
        case EditorMessage::SetComponent: {
            core::ByteReader r(payload);
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            std::uint64_t hash = 0;
            std::uint32_t blob_len = 0;
            std::span<const std::byte> blob;
            if (r.u32(index) && r.u32(generation) && r.u64(hash) && r.u32(blob_len) &&
                r.bytes(blob, blob_len)) {
                (void)editorhost::apply_set_component(
                    world, ecs::Entity{index, generation}, hash, blob);
            }
            break;
        }
        case EditorMessage::Spawn:
            (void)world.spawn();
            break;
        case EditorMessage::Despawn: {
            core::ByteReader r(payload);
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            if (r.u32(index) && r.u32(generation)) {
                (void)world.despawn(ecs::Entity{index, generation});
            }
            break;
        }
        case EditorMessage::AddComponent:
        case EditorMessage::RemoveComponent: {
            core::ByteReader r(payload);
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            std::uint64_t hash = 0;
            if (r.u32(index) && r.u32(generation) && r.u64(hash)) {
                const ecs::Entity e{index, generation};
                if (static_cast<EditorMessage>(type) == EditorMessage::AddComponent) {
                    (void)editorhost::add_default_component(world, e, hash);
                } else {
                    (void)editorhost::remove_component(world, e, hash);
                }
            }
            break;
        }
        default:
            break; // an engine->editor, RequestSnapshot (handled by the sender), or unknown type
    }
}

// Serve the editor channel over `conn` against `world`, GPU-free: schema + snapshot, then apply
// edits until the client says Bye / disconnects. This is the exact editorhost::EditorHost path.
int serve_channel(stream::ProtocolConnection conn, ecs::World& world) {
    editorhost::EditorHost host(std::move(conn));
    if (!host.send_hello(world)) {
        RIME_ERROR("rime-engine: failed to send schema + snapshot");
        return 1;
    }
    while (host.poll_one(world)) {
    }
    fmt::print("rime-engine: editor session closed ({} entities)\n", world.entity_count());
    return 0;
}

// ── Streamed viewport (needs a GPU) ───────────────────────────────────────────────────────

// A compact, lit scene to render into the viewport: a row of metallic spheres of rising roughness
// over a floor, a point light, and a camera framing them. Uses WorldTransform directly (a flat
// scene) — the same components the editor snapshots and edits, so moving a sphere from the
// inspector changes what the next frame renders.
void build_viewport_scene(ecs::World& world,
                          render::MeshRegistry& meshes,
                          render::MaterialRegistry& materials) {
    using ecs::WorldTransform;
    ecs::register_transform_components(world);
    render::register_render_components(world);

    const render::MeshId sphere = meshes.add(render::make_uv_sphere(0.8f, 32, 64), "sphere");
    const render::MeshId floor = meshes.add(render::make_plane(10.0f, 4.0f), "floor");

    for (int c = 0; c < 4; ++c) {
        const float rough = 0.08f + 0.85f * static_cast<float>(c) / 3.0f;
        render::PbrMaterialDesc m{};
        m.base_color[0] = 0.95f;
        m.base_color[1] = 0.64f;
        m.base_color[2] = 0.54f; // copper
        m.metallic = 1.0f;
        m.roughness = rough;
        core::Transform tf{};
        tf.translation = {(static_cast<float>(c) - 1.5f) * 2.0f, 0.0f, 0.0f};
        (void)world.spawn_with(
            WorldTransform{tf}, render::MeshRef{sphere}, render::MaterialRef{materials.add(m)});
    }

    render::PbrMaterialDesc floor_mat{};
    floor_mat.base_color[0] = 0.30f;
    floor_mat.base_color[1] = 0.30f;
    floor_mat.base_color[2] = 0.33f;
    floor_mat.metallic = 0.0f;
    floor_mat.roughness = 0.8f;
    core::Transform floor_tf{};
    floor_tf.translation = {0.0f, -1.6f, 0.0f};
    (void)world.spawn_with(WorldTransform{floor_tf},
                           render::MeshRef{floor},
                           render::MaterialRef{materials.add(floor_mat)});

    core::Transform cam{};
    cam.translation = {0.0f, 1.2f, 8.0f};
    cam.rotation = core::quat_from_axis_angle(core::Vec3{1.0f, 0.0f, 0.0f}, -0.12f); // pitch down
    (void)world.spawn_with(WorldTransform{cam}, render::Camera{});

    core::Transform light{};
    light.translation = {3.0f, 4.0f, 4.0f};
    (void)world.spawn_with(WorldTransform{light},
                           render::PointLight{1.0f, 0.94f, 0.85f, 120.0f, 30.0f});
}

int serve_viewport(std::string_view socket_path) {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kViewportWidth, kViewportHeight};
    cfg.tick_hz = 60.0;
    app::Application app(cfg);
    if (app.device() == nullptr) {
        // No Vulkan device (a GPU-less host): serve the editor channel without a viewport rather
        // than fail. The editor still gets the schema + snapshot + edits; its viewport shows a
        // placeholder.
        RIME_WARN("rime-engine: no Vulkan device — serving the editor channel without a viewport");
        register_and_populate(app.world(), {});
        auto listener = platform::LocalListener::bind(socket_path);
        if (!listener) {
            RIME_ERROR("rime-engine: could not bind '{}'", socket_path);
            return 1;
        }
        fmt::print("rime-engine: editor host listening on {} (no viewport)\n", socket_path);
        std::fflush(stdout);
        auto sock = listener->accept();
        if (!sock) {
            return 1;
        }
        stream::ProtocolConnection conn(std::move(*sock));
        return conn.handshake() ? serve_channel(std::move(conn), app.world()) : 1;
    }

    // Build the renderable scene and wire the per-frame render into the app's graph.
    render::MeshRegistry meshes(*app.device());
    render::MaterialRegistry materials;
    render::SceneRenderer renderer(*app.device(), meshes, materials);
    build_viewport_scene(app.world(), meshes, materials);
    renderer.set_ambient(0.03f, 0.03f, 0.04f);
    render::RGTexture last_ldr{};
    app.on_render([&](app::FrameContext& ctx) {
        last_ldr = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
    });

    auto streamer = stream::FrameStreamer::create(*app.device(), cfg.render_extent);
    if (!streamer) {
        RIME_ERROR("rime-engine: could not create the frame streamer");
        return 1;
    }

    auto listener = platform::LocalListener::bind(socket_path);
    if (!listener) {
        RIME_ERROR("rime-engine: could not bind editor-host socket '{}'", socket_path);
        return 1;
    }
    fmt::print("rime-engine: editor host + viewport on {} ({}x{}, '{}')\n",
               socket_path,
               kViewportWidth,
               kViewportHeight,
               app.device()->adapter().name.c_str());
    std::fflush(stdout);

    auto sock = listener->accept();
    if (!sock) {
        RIME_ERROR("rime-engine: accept failed");
        return 1;
    }
    stream::ProtocolConnection conn(std::move(*sock));
    if (!conn.handshake()) {
        RIME_ERROR("rime-engine: protocol handshake failed");
        return 1;
    }

    // Editor channel opener: schema then the world snapshot (sent on this, the render/send thread).
    if (!conn.send_message(static_cast<stream::MessageType>(editorhost::EditorMessage::Schema),
                           editorhost::serialize_schema(app.world())) ||
        !conn.send_message(static_cast<stream::MessageType>(editorhost::EditorMessage::Snapshot),
                           editorhost::serialize_world(app.world()))) {
        RIME_ERROR("rime-engine: failed to send schema + snapshot");
        return 1;
    }

    // Receiver thread: drain the client's edits into a queue (the render thread applies them). One
    // receiver + one sender on a full-duplex socket is the concurrency ProtocolConnection allows.
    struct Edit {
        stream::MessageType type;
        std::vector<std::byte> payload;
    };

    std::mutex mutex;
    std::vector<Edit> pending;
    std::atomic<bool> stop{false};
    std::thread receiver([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            stream::MessageType type{};
            std::vector<std::byte> payload;
            if (!conn.recv_message(type, payload) || type == stream::MessageType::Bye) {
                stop.store(true, std::memory_order_relaxed);
                break;
            }
            std::lock_guard<std::mutex> lock(mutex);
            pending.push_back({type, std::move(payload)});
        }
    });

    // Render + stream loop, paced to ~30 fps. Each frame: apply queued edits, tick + render,
    // capture the rendered LDR, LZ4-compress it, and send it as a Frame message.
    stream::FrameEncoder encoder;
    stream::ImageDesc frame_desc{};
    frame_desc.extent = cfg.render_extent;
    frame_desc.format = rhi::Format::RGBA8Unorm;
    std::uint64_t sequence = 0;
    const auto frame_period = std::chrono::milliseconds(33);
    auto next_frame = std::chrono::steady_clock::now();

    while (!stop.load(std::memory_order_relaxed)) {
        bool snapshot_requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const Edit& e : pending) {
                // RequestSnapshot has no world effect — it asks us (the send-owning thread) to
                // reply with a fresh snapshot. Apply all real edits first, then send one snapshot
                // that reflects them (coalescing multiple requests in a batch).
                if (static_cast<editorhost::EditorMessage>(e.type) ==
                    editorhost::EditorMessage::RequestSnapshot) {
                    snapshot_requested = true;
                } else {
                    apply_edit(app.world(), e.type, e.payload);
                }
            }
            pending.clear();
        }
        if (snapshot_requested && !conn.send_message(static_cast<stream::MessageType>(
                                                         editorhost::EditorMessage::Snapshot),
                                                     editorhost::serialize_world(app.world()))) {
            break; // client disconnected
        }

        app.step(app.fixed_dt()); // tick + render (executes the graph)

        const rhi::TextureHandle ldr = app.graph()->physical(last_ldr);
        const stream::FrameView view = streamer->capture(ldr);
        stream::FrameMessage frame;
        frame.sequence = sequence++;
        frame.codec = stream::Codec::LZ4;
        frame.desc = frame_desc;
        if (!encoder.encode(stream::Codec::LZ4, frame_desc, view.pixels, frame.data)) {
            RIME_ERROR("rime-engine: frame encode failed");
            break;
        }
        if (!conn.send_frame(frame)) {
            break; // client disconnected
        }

        next_frame += frame_period;
        std::this_thread::sleep_until(next_frame);
    }

    stop.store(true, std::memory_order_relaxed);
    receiver.join();
    // Synchronous capture() completes each frame before returning, so nothing is in flight to
    // drain; the streamer (and its readback buffers) release cleanly ahead of the device at scope
    // exit.
    fmt::print("rime-engine: viewport session closed after {} frames ({} entities)\n",
               sequence,
               app.world().entity_count());
    return 0;
}

// ── Entry ─────────────────────────────────────────────────────────────────────────────────

int serve(std::string_view socket_path, std::string_view scene_path, bool viewport) {
    if (viewport) {
        return serve_viewport(socket_path);
    }
    ecs::World world;
    register_and_populate(world, scene_path);
    auto listener = platform::LocalListener::bind(socket_path);
    if (!listener) {
        RIME_ERROR("rime-engine: could not bind editor-host socket '{}'", socket_path);
        return 1;
    }
    fmt::print("rime-engine: editor host listening on {} ({} entities)\n",
               socket_path,
               world.entity_count());
    std::fflush(stdout);
    auto sock = listener->accept();
    if (!sock) {
        RIME_ERROR("rime-engine: accept failed");
        return 1;
    }
    stream::ProtocolConnection conn(std::move(*sock));
    if (!conn.handshake()) {
        RIME_ERROR("rime-engine: protocol handshake failed");
        return 1;
    }
    return serve_channel(std::move(conn), world);
}

} // namespace

int main(int argc, char** argv) {
    std::string_view socket_path;
    std::string_view scene_path;
    bool viewport = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--editor-host" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--scene" && i + 1 < argc) {
            scene_path = argv[++i];
        } else if (arg == "--viewport") {
            viewport = true;
        }
    }

    if (socket_path.empty()) {
        fmt::print(
            stderr,
            "usage: rime-engine --editor-host <socket> [--scene <file.rscene>] [--viewport]\n");
        return 2;
    }
    return serve(socket_path, scene_path, viewport);
}
