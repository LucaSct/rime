// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <functional>
#include <span>
#include <string_view>

#include "rime/platform/event.hpp"
#include "rime/stream/frame_codec.hpp"

namespace rime::ecs {
class World;
}

namespace rime::editorhost {
struct HostedScene;
} // namespace rime::editorhost

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

// One fixed tick of a LIVE PLAY SESSION, with the client's input for that frame (m18 Track H,
// ADR-0054).
//
// WHY THE HOST NEEDS A THIRD CALLBACK. The two above answer "what are this game's components" and
// "what do they look like"; this one answers "what does this game DO when someone presses a key",
// and until m18 nobody could ask it, because no input ever reached the host at all. The drain loop
// now decodes `stream::InputEvent` into `platform::Event`s and posts them through
// `Application::post_input`, so `events` here is exactly `Application::frame_input()` — the same
// span a windowed game reads from its own OS pump (ADR-0023 §5), which is what makes a game's
// implementation of this hook identical to the one it already has in its windowed main().
//
// Called from the fixed-tick hook, so it runs ONLY while the session is Playing or stepping (an
// Edit-mode editor must not have WASD moving the scene under the author's cursor), after physics
// has stepped and with the world's WorldTransforms current. `dt` is the fixed tick's dt.
//
// Default `{}` means "this host has no play behaviour" — `rime-engine` opening arbitrary content
// has none to have, and every host that existed before m18 keeps its exact behaviour.
using PlayTick = std::function<void(ecs::World&, std::span<const platform::Event>, double)>;

// What the viewport host can ENCODE, in no particular preference order — `stream::choose_codec`
// walks the *client's* preference list against this one, because only the client knows whether it
// is a browser on a WAN link that wants AV1's bandwidth or a local editor that wants LZ4's
// losslessness (ADR-0030 §4). Exposed here, rather than left a local constant inside
// `run_editor_host`, so a test can assert what a real client actually gets: the negotiation is a
// contract with two live clients (the Rust editor and a browser), not an implementation detail.
inline constexpr std::array<stream::Codec, 4> kEditorHostCodecs{stream::Codec::LZ4,
                                                                stream::Codec::Av1,
                                                                stream::Codec::Jpeg,
                                                                stream::Codec::Raw};

// Parse `--editor-host <socket> [--scene <file>] [--assets <manifest>] [--viewport]` and serve
// until the client disconnects. `usage_name` is what the usage line calls this binary, so a game's
// host does not tell its users to run `rime-engine`.
// Load a --scene the way the editor host must (E3): an unknown component type is skipped and
// counted, and a failed load is recorded in `hosted` (load_ok / load_error) rather than only
// logged, so the SceneLoadReport can say so. Returns false on a failed load; the world keeps what
// loaded.
bool load_scene_for_editor(ecs::World& world,
                           std::string_view scene_path,
                           editorhost::HostedScene& hosted);

[[nodiscard]] int run_editor_host(int argc,
                                  char** argv,
                                  const ComponentRegistrar& registrar,
                                  const char* usage_name = "rime-engine",
                                  const ScenePreparer& prepare = {},
                                  const PlayTick& play_tick = {});

} // namespace rime::app
