// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "rime/app/content_root.hpp"
#include "rime/app/game_definition.hpp"
#include "rime/render/render_graph.hpp" // RGTexture (the last frame, for a caller's readback)
#include "rime/rhi/types.hpp"           // Extent2D only — no Vulkan, no backend

// Launch modes and the runner (m20.1, ADR-0056-m20.1): the engine-owned `main` of a shipped game.
//
// ADR-0046 §1 fixes the shape — ONE binary, and its end user picks the mode on the command line:
//
//     mygame [play | dedicated | browser | stream | host] [--port N] [--ticks N] [--frames N]
//            [--autopilot] [--headless] [--workers N] [--content DIR]
//     mygame dedicated [--listen ADDR:PORT | --port N | --offline]
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
//   dedicated  REAL  — no window, no device, no presentation; listens for one network input client
//                      by default, or runs the earlier local loop with --offline.
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
inline constexpr std::uint16_t kDefaultListenPort = 7463;

enum class LaunchMode : std::uint8_t { Play, Dedicated, Browser, Stream, Host };
enum class SpawnFault : std::uint8_t {
    None,
    Refuse,
    LoseOne,
    LoseAll,
    Duplicate,
    LateAlive,
    LateEnded
};
enum class SpawnDebugBreak : std::uint8_t { None, NoLedger, NoAdopt, AuthorityPresentation };
// m20.6c sensitivity controls for the interaction group (ADR-0106 §5): each removes one defence so
// the proof can show its gate going red. PresentMirror shows the mirrored (round-trip-old) object
// while predicting; NoRebuild reuses the prediction physics world across restores; StaleRestore
// restores the object from the PREVIOUS checkpoint's bytes.
enum class InteractionDebugBreak : std::uint8_t { None, PresentMirror, NoRebuild, StaleRestore };

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
    // Dedicated listens here by default. --port changes the port on 0.0.0.0; --listen can change
    // both host and port. A port of 0 asks the OS to choose and is reported in the ready event.
    // Other modes still parse --port as before; they do not bind it in this brick.
    std::string listen_host = "0.0.0.0";
    std::uint16_t port = kDefaultListenPort;
    bool offline = false;
    bool explicit_listen = false;
    // > 0: bound by this many fixed ticks. Listening dedicated runs them at the real tick rate;
    // offline dedicated and play keep the deterministic, unpaced one-tick-per-step path.
    std::uint64_t ticks = 0;
    // > 0 (play only): run exactly this many real-clock frames, then stop — the off-screen render
    // check's shape. Ignored when `ticks` is set.
    int frames = 0;
    // Drive the game from its own `Game::autopilot` instead of the keyboard (play) or no one
    // (dedicated). Refused if the game ships no autopilot.
    bool autopilot = false;
    // play only: render off-screen instead of opening a window. (Dedicated renders nothing at all.)
    bool headless = false;
    std::string connect_host;
    std::uint16_t connect_port = kDefaultListenPort;
    bool no_gpu = false;
    bool no_prediction = false;
    bool input_tape = false;
    bool input_tape_throw = false;
    // m20.6b: the game's interaction tape (walks the player into its pushable object).
    bool input_tape_push = false;
    SpawnFault spawn_fault = SpawnFault::None;
    // The window of a timed fault, in server ticks: how long `duplicate` doubles checkpoints and
    // holds the client's ack, or how long `late-*` withholds the verdict. Required by exactly
    // those three cases, so the proof states the number it derived instead of inheriting one.
    std::uint32_t spawn_fault_ticks = 0;
    // `dedicated --input-slack N` (1..8): commands buffered before one executes. 0 = unset (1).
    std::uint32_t input_slack = 0;
    SpawnDebugBreak spawn_debug_break = SpawnDebugBreak::None;
    // m20.6c, all OFF by default. `--interaction-fault kick@T[,kick@T2]` (dedicated): at those
    // server ticks the game applies its declared server-only impulse to the interaction object.
    // `--interaction-debug-break` (play): a sensitivity control. `--interaction-trace` (play): list
    // every member->member checkpoint correction in the `finished` line.
    std::vector<std::uint64_t> interaction_kicks;
    InteractionDebugBreak interaction_debug_break = InteractionDebugBreak::None;
    bool interaction_trace = false;
    std::uint32_t spawn_unresolved_bound = 0;
    std::uint32_t net_delay_ms = 0;
    std::uint32_t net_jitter_ms = 0;
    std::uint32_t net_blackout_ms = 0;
    std::uint64_t net_blackout_tick = 0;
    double net_loss = 0.0;
    std::uint64_t net_seed = 0;
    bool net_impair = false;
    double snap_bound = 0.0;
    bool has_snap_bound = false;
    std::uint32_t profile_version_override = 0;
    bool has_profile_version_override = false;
    double fixed_dt_override = 0.0;
    bool has_fixed_dt_override = false;
    std::uint16_t client_port = 0;
    std::uint32_t linger_ms = 0;
    // JobSystem workers; 0 = hardware_concurrency()-1 (AppConfig's rule).
    unsigned workers = 0;
    // --content: the content root, overriding the search (`rime/app/content_root.hpp`). Empty =
    // search.
    std::string content_dir;
    // Listening dedicated can record the action handed to each fixed tick for an independent
    // replay.
    std::string input_trace;
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

    struct NetworkInputCounters {
        std::uint64_t accepted = 0, late = 0, future = 0, duplicate = 0;
        std::uint64_t applied = 0, synthesized = 0, neutralized = 0;
        std::uint64_t truncated = 0, bad_length = 0, bad_count = 0, unknown_codec = 0;
        std::uint64_t reserved_nonzero = 0, non_finite = 0, sequence_order = 0;
        std::uint64_t hello_malformed = 0, hello_version = 0, hello_schema = 0;
        std::uint64_t clients_admitted = 0, clients_refused = 0, clients_left = 0;
        std::uint64_t refusal_close_deferred = 0;
        std::uint64_t unadmitted_messages = 0;
        std::uint64_t wrong_channel = 0, amplification_blocked = 0;
        std::uint64_t driver_dropped = 0, connect_requests_seen = 0;
        std::uint64_t oversized = 0;
        std::uint64_t replication_acks = 0, replication_ack_refused = 0;
        std::uint64_t replication_records_too_large = 0;
        std::uint64_t replication_budget_deferred = 0, replication_orphans = 0;
        std::uint64_t replication_clients_released = 0;
        std::uint64_t checkpoint_encoded = 0, checkpoint_encode_refused = 0;
        std::uint64_t checkpoint_send_refused = 0, input_result_send_refused = 0;
        std::uint64_t replication_registration_refused = 0, publish_missing_session = 0;
        std::uint64_t checkpoint_capture_refused = 0;
        // m20.5b speculative spawn (only reported when the game declares SpawnPredictionProfile).
        // Every skip, drop and refusal on the outcome path has one (guardrail 5).
        std::uint64_t spawn_accepted = 0, spawn_rejected = 0, spawn_accepted_ended = 0;
        std::uint64_t spawn_epoch_advances = 0;
        // Outcomes held back because one checkpoint carries at most 16; they go in a later one.
        std::uint64_t spawn_outcomes_deferred = 0;
        // Outcomes not retained at all: the unacknowledged list was at its hard bound.
        std::uint64_t spawn_outcomes_dropped = 0;
        // Reports the server could not turn into outcomes: a key that is not the admitted
        // session's, one not above the previous outcome, an end for an unknown entity, a launch
        // the replicator could not name (or named in the provisional range), or no client.
        std::uint64_t spawn_foreign_key = 0, spawn_out_of_order = 0, spawn_end_unknown = 0;
        std::uint64_t spawn_roster_refused = 0, spawn_without_client = 0;
        std::uint64_t spawn_extension_refused = 0;
        // SpawnOutcomeAck: applied, and every reason one was refused.
        std::uint64_t spawn_acks_applied = 0, spawn_ack_malformed = 0, spawn_ack_wrong_channel = 0;
        std::uint64_t spawn_ack_wrong_incarnation = 0, spawn_ack_ahead = 0, spawn_ack_stale = 0;
        // Process-proof faults are after session decoding and inert unless explicitly selected.
        std::uint64_t spawn_fault_refuse = 0, spawn_fault_lose_one = 0;
        std::uint64_t spawn_fault_lose_all = 0, spawn_fault_duplicate = 0;
        std::uint64_t spawn_fault_late_alive = 0, spawn_fault_late_ended = 0;
        std::uint64_t launch_recovered_by_redundancy = 0;
        // Server ticks between the most recent keyed launch's report and its end's report.
        std::uint64_t spawn_last_flight_ticks = 0;
        // m20.6b interaction group (reported only when the game's interaction profile is
        // active). The policy's own counters -- joins, leaves, a join while touching, and leaves
        // deferred because the object still moved or was still touched -- plus the epoch advances
        // they caused, the ticks the object spent in the roster, and how often its capture
        // refused a checkpoint (guardrail 5: every skip has a counter).
        std::uint64_t interaction_joins = 0, interaction_leaves = 0;
        std::uint64_t interaction_joined_in_contact = 0;
        std::uint64_t interaction_leave_deferred_moving = 0;
        std::uint64_t interaction_leave_deferred_touching = 0;
        std::uint64_t interaction_epoch_advances = 0, interaction_member_ticks = 0;
        std::uint64_t interaction_capture_refused = 0;
        // m20.6c: leaves delayed only by the quiet window; joins taken while the object still
        // moved; server-only kicks applied (and whether the object was a member at that tick).
        std::uint64_t interaction_leave_deferred_quiet = 0;
        std::uint64_t interaction_joined_sliding = 0;
        std::uint64_t interaction_kicks = 0, interaction_kicks_member = 0;
        std::uint64_t interaction_kicks_nonmember = 0;
    } network_input;

    bool bind_failed = false;
    bool usage_error = false;
    std::string bind_error;

    // The content root this run used (m20.2) — or, on a Failed run that could not find one, every
    // candidate tried (`content.tried`). `content.source == None` and `content.ok == false` with an
    // empty `tried` means the game declares no content and nothing was searched.
    ContentRoot content;
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

    // Print the one line saying which content root was chosen and why. On by default — an operator
    // must be able to see it — and off for a caller that runs the game many times and reports
    // `RunReport::content` itself (a self-check that is meant to be silent).
    bool log_content_root = true;
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
