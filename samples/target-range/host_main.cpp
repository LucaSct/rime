// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// `target-range-host` — the target range's own editor host, and the binary without which M15's
// "authored through the editor" clause is not achievable for this game.
//
// **THIS FILE EXISTS BECAUSE THE ENGINE CORRECTLY REFUSED.** Pointing the editor at the generic
// `rime-engine` and asking it to save `target_range.rscene` produces:
//
//     refusing to save: this build did not understand 5 component(s) in the scene it loaded,
//     and saving would delete them. Open it in a build that registers them.
//
// That refusal is a feature, not an obstacle. `rime-engine` registers the engine's components and
// the block's; it has never heard of `targetrange::Target`, so a save through it would silently
// drop every crate's points, its `required` flag and its knock impulse — the authored data the game
// is about. The engine noticing and stopping is the behaviour you want from a tool that owns your
// content.
//
// The fix is the seam m15.2 built for exactly this: `run_editor_host` is a LIBRARY, so a game that
// wants the editor to understand it builds its own host and answers two questions there — what its
// components ARE (the registrar) and what they LOOK LIKE (the preparer). One CMake target, no
// plugin system, and nothing in `engine/` knows this game exists. The editor already takes
// `--engine <path>`, so pointing it here instead is the whole integration.
//
// `the-block-host` is the same three lines for the block
// (`samples/99-the-block/block_host_main.cpp`). That two unrelated games need the same three lines
// and no engine change is the platform claim.

#include <vector>

#include "rime/app/editor_host_app.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/components.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/worldkit/profile.hpp"
#include "target.hpp"

namespace {

// Give the authored crates something to draw, so opening the range in the editor shows a range.
//
// A `.rscene` names a mesh by content id and this game ships no cooked assets — that is M16's
// subject, not M15's — so the crates carry a `Target` and a collider and no `MeshRef` at all.
// Without this the editor would load five invisible entities and the gizmo would have nothing to
// grab. The box is built from the collider's own half-extents rather than a constant, so a crate
// resized in the editor draws at its new size instead of quietly keeping the old silhouette.
void prepare_range_for_viewport(rime::ecs::World& world,
                                rime::render::MeshRegistry& meshes,
                                rime::render::MaterialRegistry& materials) {
    rime::render::PbrMaterialDesc crate{};
    crate.base_color[0] = 0.70f;
    crate.base_color[1] = 0.45f;
    crate.base_color[2] = 0.20f;
    crate.roughness = 0.6f;
    const rime::render::MaterialId crate_material = materials.add(crate);

    // Collect-then-add: `add_component` relocates the entity between archetypes, which would
    // invalidate the query mid-iteration.
    std::vector<std::pair<rime::ecs::Entity, rime::render::MeshId>> to_mesh;
    std::size_t already_drawn = 0;
    world.query<targetrange::Target, rime::physics::Collider>().for_each(
        [&](rime::ecs::Entity e, targetrange::Target&, rime::physics::Collider& c) {
            if (world.get<rime::render::MeshRef>(e) != nullptr) {
                ++already_drawn;
                return;
            }
            to_mesh.emplace_back(
                e,
                meshes.add(rime::render::make_box(rime::core::Vec3{c.half_x, c.half_y, c.half_z}),
                           "target"));
        });
    for (const auto& [e, mesh] : to_mesh) {
        (void)world.add_component(e, rime::render::MeshRef{mesh});
        (void)world.add_component(e, rime::render::MaterialRef{crate_material});
    }
    RIME_INFO("target-range-host: {} targets given geometry ({} already had some)",
              to_mesh.size(),
              already_drawn);
}

} // namespace

int main(int argc, char** argv) {
    return rime::app::run_editor_host(
        argc,
        argv,
        [](rime::ecs::World& world) {
            (void)rime::worldkit::register_engine_components(world); // the engine's
            targetrange::register_content_components(world);         // this game's
        },
        "target-range-host",
        prepare_range_for_viewport);
}
