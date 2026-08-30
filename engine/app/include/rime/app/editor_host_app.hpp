// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <functional>

namespace rime::ecs {
class World;
}

namespace rime::render {
class MeshRegistry;
class MaterialRegistry;
} // namespace rime::render

// The editor host, as a LIBRARY a game builds its own binary from (m15.2, ADR-0038).
//
// WHY THIS EXISTS. `rime-engine` used to register `blockkit` — the vision demo's content module —
// into the engine's own editor host, because that was the only way to make the shipped block
// inspectable. It also meant a second game could not see its own components in the inspector
// without editing engine C++ and rebuilding the engine, which is precisely the "without forking the
// engine" clause VISION §5 asks for and M15 is measured against.
//
// The fix needs no plugin system, and that is the point. `engine/editorhost` was already a library
// and the editor already accepts `--engine <path>`, so the composition point can simply be a
// BINARY: the engine ships a host that knows the engine's components, a game ships a host that
// knows its own as well, and the editor is told which to launch. One CMake target per game, no
// runtime loading, no mutable global registry — the same reasoning `worldkit` uses for the profile
// itself (guardrails 2 and 4). ADR-0038 leaves the `core` module-loader route open for when a game
// wants to add components without a rebuild.
namespace rime::app {

// Register the component set this host serves. Called once per world the host builds — the built-in
// demo world, a `--scene` load, and the streamed viewport all go through it, so a host cannot
// accidentally serve two different sets.
//
// A game's implementation is two lines: the engine's profile, then its own.
//
//     worldkit::register_engine_components(world);
//     mygame::register_mygame_components(world);
//
// A host that registers too little still WORKS: `scene::LoadOptions::allow_unknown_components` lets
// the load skip what it does not know and counts it (m14.1), so the engine's own host opens a
// game's scene degraded — every entity present, the unknown components dropped, the count reported.
// That degradation is the difference between the two hosts, and it is observable rather than
// silent.
using ComponentRegistrar = std::function<void(ecs::World&)>;

// Make the loaded world DRAWABLE (m15.4). Called once, in the viewport path only, after the scene
// has loaded and before the first frame — with the registries a game may add meshes and materials
// to.
//
// WHY A GAME HAS TO BE ASKED. A `.rscene` stores components, and a component is not geometry. The
// engine can close that gap on its own exactly when the scene names a cooked mesh by asset id —
// `render::MeshAsset`, resolved through `GpuAssetBridge` (m15.1), which the viewport now does for
// every host. But a game is free to *derive* its appearance instead, and the vision demo does: its
// 213 slabs carry a `blockkit::SlabRole` and nothing else, and `blockkit::apply_palette` turns that
// into MeshRef/MaterialRef at startup. No amount of engine cleverness can guess that mapping.
//
// So the engine asks. This is the same reasoning as `ComponentRegistrar` one step further along: a
// host binary is where a game tells the editor about itself, first what its components ARE and now
// what they LOOK LIKE. Both are ordinary C++ in the game's own target — nothing here loads a
// plugin, and nothing in `engine/` names a game.
//
// Default `{}` means "the scene is already drawable, or it is not, and either way the engine has
// nothing to add" — the honest v1 behaviour for `rime-engine` opening arbitrary content.
using ScenePreparer =
    std::function<void(ecs::World&, render::MeshRegistry&, render::MaterialRegistry&)>;

// Parse `--editor-host <socket> [--scene <file>] [--assets <manifest>] [--viewport]` and serve
// until the client disconnects. `usage_name` is what the usage line calls this binary, so a game's
// host does not tell its users to run `rime-engine`.
[[nodiscard]] int run_editor_host(int argc,
                                  char** argv,
                                  const ComponentRegistrar& registrar,
                                  const char* usage_name = "rime-engine",
                                  const ScenePreparer& prepare = {});

} // namespace rime::app
