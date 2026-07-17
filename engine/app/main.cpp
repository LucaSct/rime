// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The launcher. Born at Milestone 0 as a toolchain smoke test (print the engine name and version);
// growing, milestone by milestone, into the real application entry point. Today it also loads a
// `.rscene` scene file end-to-end — the M9.2 "loadable by the app, with or without an editor"
// proof.

#include <fmt/core.h>

#include <filesystem>
#include <string_view>

#include "rime/core/diagnostics/log.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/version.hpp"
#include "rime/ecs/reflect.hpp"   // register_transform_components
#include "rime/ecs/transform.hpp" // propagate_transforms
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp" // register_render_components
#include "rime/scene/scene_format.hpp"

namespace {

// Load `path` into a fresh world and report what came back. Registers the standard authorable
// component set first (transforms + hierarchy + render) — a scene can only name types the app knows
// — then recomputes WorldTransform, since a scene stores the authored LocalTransform and leaves the
// derived world placement to propagate_transforms. Returns a process exit code.
int load_scene(std::string_view path) {
    rime::ecs::World world;
    rime::ecs::register_transform_components(world);
    rime::render::register_render_components(world);

    const rime::scene::LoadReport report =
        rime::scene::load_scene_file(world, std::filesystem::path(path));
    if (!report.ok) {
        RIME_ERROR("scene load failed ({}): {}", path, report.error);
        return 1;
    }

    rime::core::JobSystem jobs;
    rime::ecs::propagate_transforms(world, jobs); // derive WorldTransform from the loaded hierarchy

    fmt::print("loaded scene '{}': {} entities, {} components\n",
               path,
               report.entities,
               report.components);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    fmt::print("{} engine v{}\n", rime::core::engine_name(), rime::core::version_string());

    // `--scene <file>`: load a scene and exit (M9.2). The launcher's first real capability.
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--scene") {
            if (i + 1 >= argc) {
                RIME_ERROR("--scene needs a file path");
                return 2;
            }
            return load_scene(argv[i + 1]);
        }
    }

    fmt::print("Hello from the frost. (Milestone 0: the build lives.)\n");
    RIME_INFO("diagnostics online: log / assert / timing (M1.1).");
    fmt::print("Tip: pass --scene <file.rscene> to load a scene (M9.2).\n");
    return 0;
}
