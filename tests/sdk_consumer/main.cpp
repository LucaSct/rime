// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Out-of-tree SDK consumer (M6.8). This file is deliberately NOT part of the engine build: it is
// compiled against an *installed* Rime via find_package(rime CONFIG) to prove the SDK is usable —
// that the exported rime::* targets link, that the public headers resolve from <prefix>/include,
// and that the engine runs headless outside its own source tree. scripts/sdk-smoke.sh installs Rime
// to a throwaway prefix, configures this tiny project against it, builds, and runs it — the whole
// "Rime's SDK/package story" (ADR-0016 rule 5) proven end to end. See docs/design/sdk.md.

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <vector>

#include "rime/app/application.hpp" // rime::app  → transitively rime::render/rhi/ecs/platform/core
#include "rime/assets/cooked_reader.hpp" // rime::assets
#include "rime/editorhost/thermal_preview.hpp"
#include "rime/fluids_render/phase_surface.hpp"
#include "rime/fluids_scene/preview.hpp"

int main(int argc, char** argv) {
    // 1) The application framework. A default AppConfig is headless and GPU-free, so this runs on a
    //    machine with no display or Vulkan device. Ticking the fixed-step loop a few frames proves
    //    rime::app — and its ENTIRE transitive link closure (render, rhi, ecs, platform, core plus
    //    their third-party dependencies) — resolves and links from the installed package alone.
    rime::app::Application app;
    app.run_frames(3);

    // 2) The asset runtime. Load a cooked RMA1 mesh whose path is on argv[1] (the smoke script
    //    ships cube.rmesh next to this binary). read_mesh() fully validates the file before
    //    returning — a real use of rime::assets, not just a link check.
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <mesh.rmesh>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "sdk_consumer: cannot open '%s'\n", argv[1]);
        return 3;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);

    rime::assets::AssetError err{};
    const auto mesh = rime::assets::read_mesh(bytes, err);
    if (!mesh) {
        std::fprintf(stderr,
                     "sdk_consumer: read_mesh('%s') failed (AssetError %d)\n",
                     argv[1],
                     static_cast<int>(err));
        return 4;
    }

    // 3) Exercise the installed thermal adapter and diagnostic codec. Linking editorhost also
    //    traverses stream's static AV1 dependencies, which a render-only SDK consumer misses.
    auto& world = app.world();
    rime::fluids_scene::register_fluids_scene_components(world);
    rime::fluids_scene::ThermalCell cell;
    cell.enabled = true;
    cell.liquid_mass_fraction = 0.0;
    const auto target = world.spawn_with(cell);
    (void)world.spawn_with(rime::fluids_scene::ThermalSource{target, 10000.0, true});
    rime::fluids_scene::ThermalPreview thermal;
    if (!thermal.tick(world, 1, 1.0 / 60.0) || !thermal.field() ||
        thermal.field()->cells()[0].state().liquid_mass_fraction <= 0.0)
        return 5;
    rime::fluids_render::register_fluids_render_components(world);
    world.add_component<rime::fluids_render::ThermalSurface>(target, {true});
    if (rime::fluids_render::phase_surfaces(world, thermal).count != 1)
        return 7;
    rime::editorhost::ThermalPreviewMsg diagnostic, decoded;
    const auto encoded = rime::editorhost::serialize_thermal_preview(diagnostic);
    if (!rime::editorhost::parse_thermal_preview(encoded, decoded))
        return 6;

    std::printf("sdk_consumer OK: ticked 3 frames, loaded mesh (%u vertices, %zu indices), thermal "
                "preview and wire verified\n",
                mesh->vertex_count,
                mesh->indices.size());
    return 0;
}
