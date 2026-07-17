// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// `rime-engine --editor-host <socket> [--scene <file>]` — the engine as the editor's live host
// process (M9, ADR-0016/0031). The Rust editor (tools/editor) launches this and connects over the
// s1.4 local socket; this serves the **editor channel** against a running World: it sends the
// component schema + a full-world snapshot, then drains and applies the client's edits at a tick
// boundary (editorhost::EditorHost). One editor client, one session, then exit — the process
// isolation ADR-0016 wants (an editor crash never takes the engine with it, and vice versa).
//
// GPU-free in v1: this is the world/inspector half of "editor as a client of a live engine". The
// streamed *viewport* (render → capture → encode → Frame messages) is a later brick — it slots into
// the same connection, which is why the protocol reserved both the editor band and the frame stream
// from the start. Until then the editor draws its own placeholder viewport.

#include <fmt/core.h>

#include <cstdio>
#include <filesystem>
#include <string_view>

#include "rime/core/diagnostics/log.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/reflect.hpp" // register_transform_components (+ the transform reflection)
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/components.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/stream/protocol.hpp"

namespace {

using namespace rime;

// A small default scene so `rime-engine --editor-host <socket>` (no --scene) still hands the editor
// something to inspect: a camera, a sun, and a ground mesh with a prop parented to it. Mirrors the
// samples/07-first-light shape — the same world the m9.2 fixture holds.
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

int serve(std::string_view socket_path, std::string_view scene_path) {
    ecs::World world;
    ecs::register_transform_components(world);
    render::register_render_components(world);

    if (scene_path.empty()) {
        build_default_world(world);
    } else {
        const scene::LoadReport report =
            scene::load_scene_file(world, std::filesystem::path(scene_path));
        if (!report.ok) {
            RIME_ERROR("rime-engine: scene load failed ({}): {}", scene_path, report.error);
            return 1;
        }
        core::JobSystem jobs;
        ecs::propagate_transforms(world, jobs); // derive WorldTransform from the loaded hierarchy
    }

    // Bind the local socket, then announce readiness on stdout — the editor retries its connect
    // until the node exists, so this line is a convenience for a human, not a required handshake.
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

    editorhost::EditorHost host(std::move(conn));
    if (!host.send_hello(world)) {
        RIME_ERROR("rime-engine: failed to send schema + snapshot");
        return 1;
    }

    // Serve the session: drain and apply the editor's edits until it says Bye / disconnects. (When
    // the viewport lands this loop also pumps frames; today it is the edit drain, run on this
    // thread exactly as the m9.1 host contract requires — edits land at a tick boundary.)
    while (host.poll_one(world)) {
    }

    fmt::print("rime-engine: editor session closed ({} entities)\n", world.entity_count());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view socket_path;
    std::string_view scene_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--editor-host" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--scene" && i + 1 < argc) {
            scene_path = argv[++i];
        }
    }

    if (socket_path.empty()) {
        fmt::print(stderr,
                   "usage: rime-engine --editor-host <socket-path> [--scene <file.rscene>]\n");
        return 2;
    }
    return serve(socket_path, scene_path);
}
