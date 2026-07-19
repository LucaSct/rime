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
//     A viewport click arrives as a PickRequest; the ID-buffer pick pass (render::ScenePicker,
//     m9.6) answers it with the entity under that pixel — see the pick service in the frame loop.
//
// The connection is full-duplex: one thread sends (schema/snapshot/frames), one receives (edits).
// The World is touched only by the send/render thread — the receiver just queues raw edits, which
// the render thread applies at a frame boundary (the ECS structural-change rule, ADR-0018).

#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/assets/asset_id.hpp"
#include "rime/assets/manifest.hpp"
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
#include "rime/render/gizmo_renderer.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_picker.hpp"
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
        case EditorMessage::SpawnEntity:
            (void)editorhost::spawn_entity_from_payload(world, payload);
            break;
        default:
            break; // an engine->editor, RequestSnapshot (handled by the sender), or unknown type
    }
}

// Load a cook manifest and send it as the browser's AssetList (m9.5). A no-op if no --assets path
// was given; a warn (not a failure) if the file is missing/malformed — the editor just shows an
// empty browser rather than the session refusing to start. The AssetListEntry views borrow the
// parsed Manifest's strings, so it must outlive the send (it does — send happens before return).
void send_asset_list(stream::ProtocolConnection& conn, std::string_view assets_path) {
    if (assets_path.empty()) {
        return;
    }
    std::ifstream in(std::filesystem::path(assets_path), std::ios::binary);
    if (!in) {
        RIME_WARN("rime-engine: could not open --assets manifest '{}'", assets_path);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::optional<assets::Manifest> manifest = assets::Manifest::parse(text);
    if (!manifest) {
        RIME_WARN("rime-engine: --assets manifest '{}' is malformed", assets_path);
        return;
    }
    std::vector<editorhost::AssetListEntry> entries;
    entries.reserve(manifest->entries().size());
    for (const assets::ManifestEntry& e : manifest->entries()) {
        entries.push_back(
            {static_cast<std::uint16_t>(e.kind), e.id.value, e.source_path, e.cooked_file});
    }
    (void)conn.send_message(static_cast<stream::MessageType>(editorhost::EditorMessage::AssetList),
                            editorhost::serialize_asset_list(entries));
}

// Serve the editor channel over `conn` against `world`, GPU-free: schema + snapshot + asset list,
// then apply edits until the client says Bye / disconnects. This is the exact
// editorhost::EditorHost path.
int serve_channel(stream::ProtocolConnection conn,
                  ecs::World& world,
                  std::string_view assets_path) {
    editorhost::EditorHost host(std::move(conn));
    if (!host.send_hello(world)) {
        RIME_ERROR("rime-engine: failed to send schema + snapshot");
        return 1;
    }
    send_asset_list(host.connection(), assets_path);
    while (host.poll_one(world)) {
    }
    fmt::print("rime-engine: editor session closed ({} entities)\n", world.entity_count());
    return 0;
}

// ── Streamed viewport (needs a GPU) ───────────────────────────────────────────────────────

// A compact, lit scene to render into the viewport: a row of metallic spheres of rising roughness
// over a floor, a point light, and a camera framing them. Every posed entity carries BOTH
// LocalTransform (the authored placement the inspector and the gizmo edit) and WorldTransform (the
// derived placement the renderer reads): the app's tick already runs propagate_transforms, which
// recomputes world = local for these flat roots, so an edit to LocalTransform moves the rendered
// object next frame. This uniformity is the m9.6b fix — previously the scene authored
// WorldTransform directly, which made LocalTransform edits (the component every OTHER scene path
// edits) dead here.
void build_viewport_scene(ecs::World& world,
                          render::MeshRegistry& meshes,
                          render::MaterialRegistry& materials) {
    using ecs::LocalTransform;
    using ecs::WorldTransform;
    ecs::register_transform_components(world);
    // WorldTransform is not in the default set (it is derived state, deliberately not persisted —
    // reflect.hpp); the viewport host registers it so the renderer can read it AND so the editor's
    // snapshot shows the derived pose (the gizmo projects handles at the world position).
    (void)world.register_component<WorldTransform>();
    render::register_render_components(world);

    const render::MeshId sphere = meshes.add(render::make_uv_sphere(0.8f, 32, 64), "sphere");
    const render::MeshId floor = meshes.add(render::make_plane(10.0f, 4.0f), "floor");

    const auto place = [&world](const core::Transform& tf, auto&&... comps) {
        // local == world at spawn (flat scene, no Parent); propagate keeps them equal thereafter.
        (void)world.spawn_with(
            LocalTransform{tf}, WorldTransform{tf}, std::forward<decltype(comps)>(comps)...);
    };

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
        place(tf, render::MeshRef{sphere}, render::MaterialRef{materials.add(m)});
    }

    render::PbrMaterialDesc floor_mat{};
    floor_mat.base_color[0] = 0.30f;
    floor_mat.base_color[1] = 0.30f;
    floor_mat.base_color[2] = 0.33f;
    floor_mat.metallic = 0.0f;
    floor_mat.roughness = 0.8f;
    core::Transform floor_tf{};
    floor_tf.translation = {0.0f, -1.6f, 0.0f};
    place(floor_tf, render::MeshRef{floor}, render::MaterialRef{materials.add(floor_mat)});

    core::Transform cam{};
    cam.translation = {0.0f, 1.2f, 8.0f};
    cam.rotation = core::quat_from_axis_angle(core::Vec3{1.0f, 0.0f, 0.0f}, -0.12f); // pitch down
    place(cam, render::Camera{});

    core::Transform light{};
    light.translation = {3.0f, 4.0f, 4.0f};
    place(light, render::PointLight{1.0f, 0.94f, 0.85f, 120.0f, 30.0f});
}

int serve_viewport(std::string_view socket_path, std::string_view assets_path) {
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
        return conn.handshake() ? serve_channel(std::move(conn), app.world(), assets_path) : 1;
    }

    // Build the renderable scene and wire the per-frame render into the app's graph. The picker is
    // declared after the registry it borrows (destroyed before it — its in-flight submission may
    // still reference mesh buffers, and its destructor drains that first).
    render::MeshRegistry meshes(*app.device());
    render::MaterialRegistry materials;
    render::SceneRenderer renderer(*app.device(), meshes, materials);
    render::ScenePicker picker(*app.device(), meshes);
    render::GizmoRenderer gizmos(*app.device(), meshes);
    build_viewport_scene(app.world(), meshes, materials);
    renderer.set_ambient(0.03f, 0.03f, 0.04f);

    // The editor's gizmo state (selection + mode + hovered axis), updated by the drain below and
    // read by the render callback. Main-thread only, like the pick queue — the receiver never
    // touches it.
    render::GizmoSelection gizmo_sel{};
    // The lens the frame was rendered with — captured in the render callback, sent to the editor
    // as ViewportCamera after the frame (so the message always matches the streamed pixels).
    render::CameraLens frame_lens{};

    render::RGTexture last_ldr{};
    app.on_render([&](app::FrameContext& ctx) {
        last_ldr = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
        // One lens per frame, shared by the gizmo pass and the ViewportCamera message — computed
        // AFTER the tick (the callback runs post-tick), so it sees the same WorldTransforms the
        // frame renders.
        frame_lens = render::compute_camera_lens(ctx.world, ctx.extent);
        // The overlay draws over the finished LDR before capture/stream: the editor sees the
        // gizmo composited into the same frame its drag math targets.
        gizmos.declare(*ctx.graph, ctx.world, last_ldr, frame_lens, gizmo_sel);
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
    send_asset_list(conn, assets_path); // the browser's manifest (m9.5), if --assets was given

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

    // Pick requests waiting for the GPU. The picker serves one at a time (its 1×1 target and
    // readback are single-slot), so requests queue here and each gets exactly one PickResult —
    // clicks are human-rate, the queue is effectively 0–1 deep.
    std::deque<std::pair<std::int32_t, std::int32_t>> pick_queue;

    while (!stop.load(std::memory_order_relaxed)) {
        bool snapshot_requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const Edit& e : pending) {
                // RequestSnapshot has no world effect — it asks us (the send-owning thread) to
                // reply with a fresh snapshot. Apply all real edits first, then send one snapshot
                // that reflects them (coalescing multiple requests in a batch). PickRequest is the
                // same shape of message — answered by this thread, not applied to the world.
                const auto msg = static_cast<editorhost::EditorMessage>(e.type);
                if (msg == editorhost::EditorMessage::RequestSnapshot) {
                    snapshot_requested = true;
                } else if (msg == editorhost::EditorMessage::PickRequest) {
                    core::ByteReader r(e.payload);
                    std::uint32_t ux = 0;
                    std::uint32_t uy = 0;
                    if (r.u32(ux) && r.u32(uy)) {
                        // The wire carries i32 pixel coords; same bytes, reinterpreted. Negative /
                        // out-of-range values are legal input — begin_pick answers them as misses.
                        pick_queue.emplace_back(static_cast<std::int32_t>(ux),
                                                static_cast<std::int32_t>(uy));
                    }
                } else if (msg == editorhost::EditorMessage::GizmoState) {
                    // Engine state, not a world edit (m9.6b): remember what gizmo to render.
                    // Latest wins within a batch — each message is a complete state, so replaying
                    // stale ones would only draw frames the editor has already moved past.
                    editorhost::GizmoStateMsg gs{};
                    if (editorhost::parse_gizmo_state(e.payload, gs)) {
                        gizmo_sel.entity = ecs::Entity{gs.index, gs.generation};
                        gizmo_sel.mode =
                            gs.index == 0xFFFFFFFFu
                                ? render::GizmoMode::None
                                : static_cast<render::GizmoMode>(gs.mode <= 3 ? gs.mode : 0);
                        gizmo_sel.axis = static_cast<render::GizmoAxis>(gs.axis <= 3 ? gs.axis : 0);
                    }
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

        // The pick service (m9.6): collect a finished pick and answer it, then start the next
        // queued one against the post-tick world — the world the frame streamed below shows.
        // try_resolve is non-blocking (the s1.1 ticket), so a click never stalls the stream; its
        // answer simply arrives a frame later (the documented pick latency). kNullEntity's raw
        // handle (index 0xFFFFFFFF, generation 0) IS the wire's "nothing" sentinel, so hit and
        // miss serialize identically.
        if (picker.pending()) {
            if (const auto picked = picker.try_resolve()) {
                std::vector<std::byte> out;
                core::ByteWriter w(out);
                w.u32(picked->index);
                w.u32(picked->generation);
                if (!conn.send_message(
                        static_cast<stream::MessageType>(editorhost::EditorMessage::PickResult),
                        out)) {
                    break; // client disconnected
                }
            }
        }
        if (!picker.pending() && !pick_queue.empty()) {
            const auto [px, py] = pick_queue.front();
            pick_queue.pop_front();
            picker.begin_pick(app.world(), cfg.render_extent, px, py);
        }

        // Ship the frame's exact lens (m9.6b): the editor's gizmo hover/drag math unprojects
        // through THESE matrices, so they must be the ones the pixels below were rendered with —
        // sent every frame (~148 B, noise next to the LZ4 frame) rather than on-change, because
        // an inspector edit to the camera must reflect in the very next lens the editor holds.
        if (frame_lens.found) {
            editorhost::ViewportCameraMsg cam{};
            std::memcpy(cam.view_proj, frame_lens.view_proj.m, sizeof(cam.view_proj));
            std::memcpy(cam.inv_view_proj, frame_lens.inv_view_proj.m, sizeof(cam.inv_view_proj));
            cam.eye[0] = frame_lens.eye.x;
            cam.eye[1] = frame_lens.eye.y;
            cam.eye[2] = frame_lens.eye.z;
            cam.width = cfg.render_extent.width;
            cam.height = cfg.render_extent.height;
            if (!conn.send_message(
                    static_cast<stream::MessageType>(editorhost::EditorMessage::ViewportCamera),
                    editorhost::serialize_viewport_camera(cam))) {
                break; // client disconnected
            }
        }

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

int serve(std::string_view socket_path,
          std::string_view scene_path,
          std::string_view assets_path,
          bool viewport) {
    if (viewport) {
        return serve_viewport(socket_path, assets_path);
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
    return serve_channel(std::move(conn), world, assets_path);
}

} // namespace

int main(int argc, char** argv) {
    std::string_view socket_path;
    std::string_view scene_path;
    std::string_view assets_path;
    bool viewport = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--editor-host" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--scene" && i + 1 < argc) {
            scene_path = argv[++i];
        } else if (arg == "--assets" && i + 1 < argc) {
            assets_path = argv[++i];
        } else if (arg == "--viewport") {
            viewport = true;
        }
    }

    if (socket_path.empty()) {
        fmt::print(stderr,
                   "usage: rime-engine --editor-host <socket> [--scene <file.rscene>] "
                   "[--assets <manifest>] [--viewport]\n");
        return 2;
    }
    return serve(socket_path, scene_path, assets_path, viewport);
}
