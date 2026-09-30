// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// `the-block-host` — the vision demo's own editor host (m15.2 + m15.4, ADR-0038).
//
// THIS FILE IS THE POINT OF THE BRICK. A game that wants the editor to understand it builds its own
// host binary and answers two questions there: what its components ARE (the registrar, m15.2) and
// what they LOOK LIKE (the preparer, m15.4). One CMake target, no plugin system, and nothing in
// `engine/` knows this game exists.
//
// The editor already takes `--engine <path>`, so pointing it here instead of at `rime-engine` is
// the whole integration. `rime-engine` still opens `block.rscene` — the loader skips
// `blockkit::SlabRole` and reports it — which is exactly the degradation m14.1 built and exactly
// the difference this binary removes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rime/app/editor_host_app.hpp"
#include "rime/blockkit/block.hpp"
#include "rime/blockkit/palette.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/input_map.hpp"
#include "rime/platform/event.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/worldkit/profile.hpp"

namespace {

// Turn the block's authored data into something the viewport can draw.
//
// A `.rscene` stores components, and `blockkit::SlabRole` is not geometry — it is "building 3's
// ground-storey front wall, tint 2". `apply_palette` is the function that reads it, and at runtime
// 99-the-block calls exactly this. The editor could not, because before m15.4 there was no place
// for a game to hand the host a function; that is the gap this closes, and closing it is what makes
// "open the block and see it" possible without one line of engine code naming this game.
void prepare_block_for_viewport(rime::ecs::World& world,
                                rime::render::MeshRegistry& meshes,
                                rime::render::MaterialRegistry& materials) {
    rime::blockkit::BlockPalette palette = rime::blockkit::build_palette(materials);
    rime::blockkit::upload_prop_meshes(palette, meshes);
    const rime::blockkit::PaletteStats stats = rime::blockkit::apply_palette(world, palette);

    // THE EDITOR SHOWS THE UNDAMAGED BLOCK, and that is a deliberate difference from the running
    // game rather than an approximation of it. At runtime a wall/floor/roof/crate draws as its
    // fractured PARTS (the m13.2d leaf bridge), so `apply_palette` gives those kinds a material and
    // no mesh — correct in a world with a live DestructionWorld, and an invisible building in one
    // without. An editor has neither, and nothing has been broken yet, so the honest thing to draw
    // is the volume the fracture was CUT FROM: the source box named by the entity's `Destructible`.
    //
    // ONE MESH PER PATTERN, NOT ONE SCALED CUBE. All 140 destructible slabs are authored with an
    // identity scale — a wall's size lives in its cooked `.rdest`, not in its placement — so a
    // shared unit cube draws 140 one-metre boxes floating where the buildings should be, which is
    // what the first run of this brick actually produced. `cook_specs()` already carries the box
    // dimensions beside the content id a `Destructible` names, so nine `make_box` meshes cover the
    // whole block with no file read and no manifest. And because the size is in the MESH, dragging
    // one wall's gizmo cannot resize another.
    std::unordered_map<std::uint64_t, rime::render::MeshId> preview;
    for (const rime::blockkit::CookSpec& spec : rime::blockkit::cook_specs()) {
        preview[spec.asset] =
            meshes.add(rime::render::make_box(rime::core::Vec3{
                           spec.size_x * 0.5f, spec.size_y * 0.5f, spec.size_z * 0.5f}),
                       spec.name);
    }

    std::vector<std::pair<rime::ecs::Entity, rime::render::MeshId>> to_mesh;
    std::size_t unknown_pattern = 0;
    world.query<rime::blockkit::SlabRole, rime::destruction::Destructible>().for_each(
        [&](rime::ecs::Entity e, rime::blockkit::SlabRole&, rime::destruction::Destructible& d) {
            if (world.get<rime::render::MeshRef>(e) != nullptr) {
                return; // apply_palette already gave this kind real geometry
            }
            const auto it = preview.find(d.asset);
            if (it == preview.end()) {
                ++unknown_pattern; // a cook this build does not know — counted, never silent
                return;
            }
            to_mesh.emplace_back(e, it->second); // collect: add_component moves the archetype
        });
    for (const auto& [e, mesh] : to_mesh) {
        (void)world.add_component(e, rime::render::MeshRef{mesh});
    }

    // `missing` must be zero — a SlabRole kind the palette has no answer for is a prop that is
    // silently not there. Reported rather than asserted: a host that dies on one odd entity is
    // worse for an editor than one that draws the other 212 and says what it could not.
    RIME_INFO("the-block-host: palette applied — {} materialed, {} meshed (+{} slab previews), "
              "{} roles missing, {} unknown patterns",
              stats.materialed,
              stats.meshed,
              to_mesh.size(),
              stats.missing,
              unknown_pattern);
}

// ── Play mode: the block, flown from a browser (m18 Track H, ADR-0054) ──────────────────────────
//
// THIS IS THE OTHER HALF OF THE BRICK. The engine now decodes the browser's `stream::InputEvent`s
// into `platform::Event`s and posts them where `Application::frame_input()` can see them — but an
// engine may not decide what a game does with WASD, so `run_editor_host`'s `PlayTick` hook is
// where this game says. What it says is the simplest thing that makes rime.peekstar.eu something
// other than a video: press ▶ and you fly through the block.
//
// A FREE CAMERA AND NOT A CHARACTER, deliberately. `99-the-block`'s windowed main() spawns a
// predicted, replicated capsule with a weapon; reproducing that here would mean standing up the
// net stack inside an editor host for one local viewer. The editor's own convention for "move the
// viewpoint" is a fly camera, `gameplay::FlyCamera` already is one, and it consumes exactly the
// event span this hook is handed — the same join `tests/app/windowed_input_test.cpp` proves for a
// windowed app. A character controller is a later brick with a real reason to exist.
//
// PLAY MODE ONLY. The hook runs from the fixed tick, which only ticks while Playing/Step, so an
// author editing the scene in the Rust editor never has their camera walked out from under them —
// and `Stop` restores the pre-play snapshot, which puts the camera back where it was.
class BlockFlyCamera {
public:
    void tick(rime::ecs::World& world, std::span<const rime::platform::Event> events, double dt) {
        // The camera is a world entity, and which entity that is can change between play sessions
        // (a scene load, an author spawning another camera), so it is looked up per tick rather
        // than cached across one. One query over a handful of cameras costs nothing next to the
        // frame it precedes.
        rime::ecs::Entity camera = rime::ecs::kNullEntity;
        rime::core::Transform current{};
        world.query<rime::render::Camera, rime::ecs::LocalTransform>().for_each(
            [&](rime::ecs::Entity e, rime::render::Camera& cam, rime::ecs::LocalTransform& local) {
                if (camera == rime::ecs::kNullEntity && cam.active) {
                    camera = e;
                    current = local.value;
                }
            });
        if (camera == rime::ecs::kNullEntity) {
            return; // a scene with no active camera: nothing to fly, and nothing being rendered
        }

        // SEED FROM THE SCENE, ONCE PER SESSION. Starting the fly camera at the origin looking
        // down −Z would teleport the viewer away from the shot the scene was authored to open on,
        // the first time they nudge the mouse. Yaw/pitch are recovered from the camera's own
        // forward vector — exact, because `eye_transform` builds the rotation as yaw-then-pitch
        // and this inverts precisely that.
        if (!seeded_) {
            const rime::core::Vec3 forward =
                rime::core::rotate(current.rotation, rime::core::Vec3{0.0f, 0.0f, -1.0f});
            fly_.position = current.translation;
            fly_.view.yaw = std::atan2(-forward.x, -forward.z);
            fly_.view.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
            seeded_ = true;
        }

        // Pointer capture is the browser page's job (it calls `requestPointerLock` before it
        // forwards anything), and the page only sends input while locked — so look does NOT
        // require a held button here. This is the one place `look_requires_drag` is set from
        // knowledge the engine does not have: what the CLIENT did, not what this process asked
        // for.
        fly_.bindings.look_requires_drag = false;
        fly_.update(events, static_cast<float>(dt));

        if (auto* local = world.get<rime::ecs::LocalTransform>(camera)) {
            // Scale is preserved rather than overwritten: a camera's scale is meaningless to the
            // lens but it is the author's data, and a play session must give back what it took.
            const rime::core::Transform eye = fly_.transform();
            local->value.translation = eye.translation;
            local->value.rotation = eye.rotation;
        }
    }

private:
    rime::gameplay::FlyCamera fly_{};
    bool seeded_ = false;
};

} // namespace

int main(int argc, char** argv) {
    // One per process, outliving the callback the host holds: a fly camera that forgot its angle
    // every tick would not be a camera.
    BlockFlyCamera fly;
    return rime::app::run_editor_host(
        argc,
        argv,
        [](rime::ecs::World& world) {
            (void)rime::worldkit::register_engine_components(world); // the engine's
            rime::blockkit::register_blockkit_components(world);     // this game's
        },
        "the-block-host",
        prepare_block_for_viewport,
        [&fly](rime::ecs::World& world, std::span<const rime::platform::Event> events, double dt) {
            fly.tick(world, events, dt);
        });
}
