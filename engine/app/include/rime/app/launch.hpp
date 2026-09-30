// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "rime/app/game_definition.hpp"
#include "rime/render/render_graph.hpp" // RGTexture (the last frame, for a caller's readback)
#include "rime/rhi/types.hpp"           // Extent2D only — no Vulkan, no backend

// Launch modes and the runner (m20.1, ADR-0056-m20.1): the engine-owned `main` of a shipped game.
//
// ADR-0046 §1 fixes the shape — ONE binary, and its end user picks the mode on the command line:
//
//     mygame [play | dedicated | browser | stream | host] [--port N] [--ticks N] [--frames N]
//            [--autopilot] [--headless] [--workers N]
//
// and it fixes the ORDER: the mode is decided before any device exists, because the only way to
// guarantee a dedicated server never touches Vulkan is for the code that would create a device to
// run after the decision and to be skipped by it. So `parse_launch` is a pure function of argv that
// allocates nothing but strings, and `run_game_mode` decides `AppConfig::gpu` from the mode before
// it constructs the Application. In `dedicated`, `gpu` is false; `Application` then never enters
// the device factory at all, and the RHI's loader (volk) is only ever opened INSIDE that factory —
// so no `libvulkan` is mapped into a dedicated process. `hello_game --dedicated-proof` checks both
// of those in a running process, not just in this comment.
//
// What is real in this brick and what is a stub, stated at the top rather than discovered:
//
//   play       REAL  — a device, optionally a window, the engine's SceneRenderer, keyboard input
//   dedicated  REAL  — no window, no device, no presentation; the same game object, the same loop.
//                      Its input is the autopilot or nothing: accepting network clients is later.
//   browser    STUB  — parsed, then `RunStatus::NotYetImplemented` (ADR-0045's gateway path)
//   stream     STUB  — parsed, then `RunStatus::NotYetImplemented` (the 04-remote-view pipeline)
//   host       STUB  — parsed, then `RunStatus::NotYetImplemented` (net / replication for
//   strangers)
//
// A stub that silently ran `play` instead would be the worst of both: the user asked for a server
// and got a window. So each one says, by name, that it does not exist yet.
namespace rime::rhi {
class Device;
}

namespace rime::app {

class Application;

// The off-screen / initial window size `play` renders at. A named constant rather than a buried
// literal because a caller reading back the last frame (`RunHooks::on_finished`) needs the same
// numbers the runner used.
inline constexpr rhi::Extent2D kPlayRenderExtent{1280, 720};

enum class LaunchMode : std::uint8_t { Play, Dedicated, Browser, Stream, Host };

[[nodiscard]] std::string_view to_string(LaunchMode mode) noexcept;

// Does this mode create an RHI device? The single place that answer lives — the runner asks it, and
// the test asks it — so "which modes may touch the GPU" is one line to review, not a property
// smeared across a switch statement in each mode's code.
[[nodiscard]] constexpr bool mode_uses_device(LaunchMode mode) noexcept {
    // browser and stream will render (they encode frames) when they exist; host is a listen-server
    // with a local player. Only `dedicated` is headless by definition, and it is the one that
    // matters: it must run on a box with no Vulkan at all.
    return mode != LaunchMode::Dedicated;
}

struct LaunchOptions {
    LaunchMode mode = LaunchMode::Play;
    // For the listening modes. Parsed and validated now so the contract is fixed before the modes
    // that use it exist; `play` and `dedicated` do not listen yet and ignore it.
    std::uint16_t port = 0;
    // > 0: a BOUNDED, DETERMINISTIC run of exactly this many fixed ticks — one tick per frame,
    // stepped with the fixed dt, no wall clock — ending early only if the game finishes. This is
    // what a proof, a soak run or a CI job wants. 0: run off the real clock until the game finishes
    // or (windowed) the window closes.
    std::uint64_t ticks = 0;
    // > 0 (play only): run exactly this many real-clock frames, then stop — the off-screen render
    // check's shape. Ignored when `ticks` is set.
    int frames = 0;
    // Drive the game from its own `Game::autopilot` instead of the keyboard (play) or no one
    // (dedicated). Refused if the game ships no autopilot.
    bool autopilot = false;
    // play only: render off-screen instead of opening a window. (Dedicated renders nothing at all.)
    bool headless = false;
    // JobSystem workers; 0 = hardware_concurrency()-1 (AppConfig's rule).
    unsigned workers = 0;
};

enum class ParseStatus : std::uint8_t { Ok, Help, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Ok;
    LaunchOptions options;
    std::string error; // set when status == Error: which argument, and why
};

// Parse `args` (argv WITHOUT the program name). The mode word, if present, must come first; with no
// mode word the mode is `play` — ADR-0046 wants a launcher UI for the no-argument case, and until
// one exists "double-click runs the game" is the honest default.
[[nodiscard]] ParseResult parse_launch(std::span<const std::string_view> args);

// The usage text, naming the game so a shipped binary does not tell its users to run `rime-game`.
[[nodiscard]] std::string launch_usage(std::string_view program);

enum class RunStatus : std::uint8_t {
    Ok,
    NotYetImplemented, // a parsed mode this build cannot run — see the table at the top
    Failed,            // the definition, the scene, or the game's setup refused
};

// What a run did. Every number here is something a proof or an operator would otherwise have to
// infer from the absence of a log line.
struct RunReport {
    RunStatus status = RunStatus::Ok;
    std::string message; // why, for NotYetImplemented / Failed

    LaunchMode mode = LaunchMode::Play;
    std::uint64_t ticks = 0;  // fixed ticks the GAME took
    std::uint64_t frames = 0; // loop iterations (frames), rendered or not
    bool finished = false;    // the game asked to stop (vs. a tick/frame bound or window close)
    bool had_device = false;  // a device existed for the run
    bool windowed = false;    // a window actually came up (not merely requested)

    // How many times the device factory was entered. Zero in `dedicated` — asserted by the tests,
    // not assumed — and at most one otherwise.
    std::uint64_t device_factory_calls = 0;

    // Ticks the loop ran AFTER the game had finished, which the runner did not pass to the game.
    // Non-zero only off the real clock, where one frame can owe several ticks and the game can
    // finish on the first of them. Counted rather than silently dropped (CLAUDE.md guardrail 5).
    std::uint64_t ticks_skipped_after_finish = 0;
    // Present hooks not called because the mode wanted a device and none could be made. Non-zero
    // means `play` degraded to a simulation-only run on this machine.
    std::uint64_t presentation_skipped = 0;
    // Bindings in the definition's InputMap the mapper refused (out-of-range index).
    std::size_t rejected_bindings = 0;

    // `Game::state_digest` of the final world, taken before teardown. 0 if the game supplies none.
    std::uint64_t state_digest = 0;
};

// Hooks for a caller that embeds the runner — a test, a self-check, a later launcher. None of them
// is part of what a game supplies; a shipped `main` passes none.
struct RunHooks {
    // Replace `rhi::create_device`. The runner wraps whatever is used here (the default included)
    // in a counter, which is what `RunReport::device_factory_calls` reports.
    std::function<std::unique_ptr<rhi::Device>()> device_factory;

    // Called once after the loop ends, GPU idle, before anything is torn down — the only moment a
    // caller can read back the last frame (`last_frame` is invalid when nothing was rendered).
    std::function<void(Application& app, Game& game, render::RGTexture last_frame)> on_finished;
};

// Run `definition` in the mode `options` names. Never throws on the frame path; a refusal is a
// report with a status and a message.
[[nodiscard]] RunReport run_game_mode(const LaunchOptions& options,
                                      const GameDefinition& definition,
                                      const RunHooks& hooks = {});

// The whole `main` of a shipped game: parse, run, print a one-line report, return an exit code —
// 0 ok, 1 failed, 2 usage, 3 not yet implemented. Distinct codes so a script (or the gateway,
// later) can tell "you asked for something this build cannot do" from "it broke".
[[nodiscard]] int run_game(int argc, char** argv, const GameDefinition& definition);

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitNotYet = 3;

} // namespace rime::app
