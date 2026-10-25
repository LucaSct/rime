// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine-owned `main` of a shipped game (m20.1, ADR-0046 §1, ADR-0056-m20.1): parse the mode,
// decide whether a device may exist, then run the ONE loop every mode shares around the game's
// `Game` object. The header states what is real and what is a stub; this file is written so the
// ordering that makes `dedicated` GPU-free can be read top to bottom in `run_game_mode`.

#include "rime/app/launch.hpp"

#include <fmt/core.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/app/network_client.hpp"
#include "rime/app/network_input.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/platform/clock.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/device.hpp"
#include "rime/scene/derive_transforms.hpp"
#include "rime/scene/scene_format.hpp"

namespace rime::app {
namespace {

// Parse an unsigned integer that must fill the whole token. `std::atoi` — what the samples used —
// turns "12abc" into 12 and "abc" into 0, and a launcher that reads `--ticks abc` as "run zero
// ticks" produces a green run that did nothing.
template <typename T> [[nodiscard]] bool parse_uint(std::string_view text, T& out) {
    if (text.empty()) {
        return false;
    }
    T value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool parse_mode(std::string_view word, LaunchMode& out) {
    if (word == "play") {
        out = LaunchMode::Play;
    } else if (word == "dedicated") {
        out = LaunchMode::Dedicated;
    } else if (word == "browser") {
        out = LaunchMode::Browser;
    } else if (word == "stream") {
        out = LaunchMode::Stream;
    } else if (word == "host") {
        out = LaunchMode::Host;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] RunReport refuse(RunReport r, RunStatus status, std::string message) {
    r.status = status;
    r.message = std::move(message);
    return r;
}

} // namespace

std::string_view to_string(LaunchMode mode) noexcept {
    switch (mode) {
        case LaunchMode::Play:
            return "play";
        case LaunchMode::Dedicated:
            return "dedicated";
        case LaunchMode::Browser:
            return "browser";
        case LaunchMode::Stream:
            return "stream";
        case LaunchMode::Host:
            return "host";
    }
    return "unknown";
}

std::string launch_usage(std::string_view program) {
    return fmt::format(
        "usage: {} [play | dedicated | browser | stream | host] [options]\n"
        "  play        play locally (the default): a window, or off-screen with --headless\n"
        "  dedicated   a UDP server by default: no window, no GPU device\n"
        "  browser     host for a browser          (not yet implemented)\n"
        "  stream      a CLI streaming server      (not yet implemented)\n"
        "  host        host a multiplayer session  (not yet implemented)\n"
        "options:\n"
        "  --ticks N     bound the run to N fixed ticks\n"
        "  --frames N    play: run N real-clock frames, then stop\n"
        "  --autopilot   drive the game from its built-in autopilot\n"
        "  --headless    play: render off-screen instead of opening a window\n"
        "  --connect HOST[:PORT]  play on a remote server (default port 7463)\n"
        "  --no-gpu     connected play without a device or window\n"
        "  --no-prediction  display the authoritative local player\n"
        "  --net-impair delay_ms=D,loss=P,jitter_ms=J,seed=S,blackout_ms=B@tick=T\n"
        "  --snap-bound M  count connected corrections larger than M metres\n"
        "  --profile-version-override N  connected schema refusal diagnostic\n"
        "  --fixed-dt-override S  advertise a different connected timestep\n"
        "  --client-port N  connected local UDP port diagnostic\n"
        "  --input-tape  connected fixed input for process diagnostics (no Throw)\n"
        "  --input-tape-throw  same tape, with one Throw on tick 12\n"
        "  --input-tape-push  connected interaction tape: walk into the crate, push, retreat\n"
        "  --spawn-fault CASE  dedicated Throw diagnostic (refuse, lose-one, lose-all, duplicate, "
        "late-alive, late-ended)\n"
        "  --spawn-debug-break CASE  connected sensitivity control (no-ledger, no-adopt, "
        "authority-presentation)\n"
        "  --spawn-fault-ticks N  window of duplicate / late-alive / late-ended, in server ticks\n"
        "  --spawn-unresolved-bound N  connected diagnostic expiry in input sequences\n"
        "  --linger-ms N  bounded dedicated: drain network after the final tick\n"
        "  --workers N   job-system worker threads (0 = one per core, less one)\n"
        "  --listen ADDR:PORT  dedicated: bind IPv4 address and port (0 = OS chosen)\n"
        "  --offline    dedicated: run without a network listener\n"
        "  --port N     dedicated: shorthand for --listen 0.0.0.0:N\n"
        "  --content DIR the game's content root (default: content/ beside this executable)\n"
        "  --input-trace PATH  listening dedicated: record the action used each tick\n",
        program);
}

ParseResult parse_launch(std::span<const std::string_view> args) {
    ParseResult result{};
    auto fail = [&](std::string message) {
        result.status = ParseStatus::Error;
        result.error = std::move(message);
        return result;
    };

    std::size_t i = 0;
    bool port_given = false;
    bool linger_given = false;
    // The mode word, if any, is FIRST. Positional-first keeps `mygame dedicated --port 7777` and
    // `mygame --port 7777 dedicated` from both being valid spellings of one command, which is how a
    // CLI grows a second, undocumented grammar.
    if (!args.empty() && !args[0].starts_with("-")) {
        if (!parse_mode(args[0], result.options.mode)) {
            return fail(fmt::format("unknown mode '{}'", args[0]));
        }
        i = 1;
    }

    for (; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--help" || arg == "-h") {
            result.status = ParseStatus::Help;
            return result;
        }
        if (arg == "--autopilot") {
            result.options.autopilot = true;
        } else if (arg == "--headless") {
            result.options.headless = true;
        } else if (arg == "--no-gpu") {
            result.options.no_gpu = true;
        } else if (arg == "--no-prediction") {
            result.options.no_prediction = true;
        } else if (arg == "--input-tape") {
            result.options.input_tape = true;
        } else if (arg == "--input-tape-throw") {
            result.options.input_tape = true;
            result.options.input_tape_throw = true;
        } else if (arg == "--input-tape-push") {
            result.options.input_tape = true;
            result.options.input_tape_push = true;
        } else if (arg == "--spawn-fault") {
            if (!has_value || result.options.spawn_fault != SpawnFault::None)
                return fail("--spawn-fault needs one case");
            const auto value = args[++i];
            if (value == "refuse")
                result.options.spawn_fault = SpawnFault::Refuse;
            else if (value == "lose-one")
                result.options.spawn_fault = SpawnFault::LoseOne;
            else if (value == "lose-all")
                result.options.spawn_fault = SpawnFault::LoseAll;
            else if (value == "duplicate")
                result.options.spawn_fault = SpawnFault::Duplicate;
            else if (value == "late-alive")
                result.options.spawn_fault = SpawnFault::LateAlive;
            else if (value == "late-ended")
                result.options.spawn_fault = SpawnFault::LateEnded;
            else
                return fail("--spawn-fault needs a known case");
        } else if (arg == "--spawn-debug-break") {
            if (!has_value || result.options.spawn_debug_break != SpawnDebugBreak::None)
                return fail("--spawn-debug-break needs one case");
            const auto value = args[++i];
            if (value == "no-ledger")
                result.options.spawn_debug_break = SpawnDebugBreak::NoLedger;
            else if (value == "no-adopt")
                result.options.spawn_debug_break = SpawnDebugBreak::NoAdopt;
            else if (value == "authority-presentation")
                result.options.spawn_debug_break = SpawnDebugBreak::AuthorityPresentation;
            else
                return fail("--spawn-debug-break needs a known case");
        } else if (arg == "--spawn-fault-ticks") {
            if (!has_value || result.options.spawn_fault_ticks != 0 ||
                !parse_uint(args[++i], result.options.spawn_fault_ticks) ||
                result.options.spawn_fault_ticks == 0)
                return fail("--spawn-fault-ticks needs a positive integer once");
        } else if (arg == "--spawn-unresolved-bound") {
            if (!has_value || result.options.spawn_unresolved_bound != 0 ||
                !parse_uint(args[++i], result.options.spawn_unresolved_bound) ||
                result.options.spawn_unresolved_bound == 0)
                return fail("--spawn-unresolved-bound needs a positive integer once");
        } else if (arg == "--snap-bound") {
            if (!has_value || result.options.has_snap_bound)
                return fail("--snap-bound needs a non-negative finite number once");
            const auto value = args[++i];
            const auto [end, ec] = std::from_chars(
                value.data(), value.data() + value.size(), result.options.snap_bound);
            if (ec != std::errc{} || end != value.data() + value.size() ||
                !std::isfinite(result.options.snap_bound) || result.options.snap_bound < 0)
                return fail("--snap-bound needs a non-negative finite number");
            result.options.has_snap_bound = true;
        } else if (arg == "--profile-version-override") {
            if (!has_value || result.options.has_profile_version_override ||
                !parse_uint(args[++i], result.options.profile_version_override))
                return fail("--profile-version-override needs an integer once");
            result.options.has_profile_version_override = true;
        } else if (arg == "--fixed-dt-override") {
            if (!has_value || result.options.has_fixed_dt_override)
                return fail("--fixed-dt-override needs a positive finite number once");
            const auto value = args[++i];
            const auto [end, ec] = std::from_chars(
                value.data(), value.data() + value.size(), result.options.fixed_dt_override);
            if (ec != std::errc{} || end != value.data() + value.size() ||
                !std::isfinite(result.options.fixed_dt_override) ||
                result.options.fixed_dt_override <= 0)
                return fail("--fixed-dt-override needs a positive finite number");
            result.options.has_fixed_dt_override = true;
        } else if (arg == "--client-port") {
            if (!has_value || !parse_uint(args[++i], result.options.client_port) ||
                result.options.client_port == 0)
                return fail("--client-port needs a port in 1..65535");
        } else if (arg == "--linger-ms") {
            if (!has_value || linger_given || !parse_uint(args[++i], result.options.linger_ms))
                return fail("--linger-ms needs a non-negative integer");
            linger_given = true;
        } else if (arg == "--connect") {
            if (!has_value || !result.options.connect_host.empty())
                return fail("--connect needs HOST[:PORT] once");
            const auto address = args[++i];
            if (address.starts_with("["))
                return fail("IPv6 is not supported yet");
            const auto colon = address.find(':');
            const auto host = address.substr(0, colon);
            if (host.empty() ||
                (colon != std::string_view::npos &&
                 (address.find(':', colon + 1) != std::string_view::npos ||
                  !parse_uint(address.substr(colon + 1), result.options.connect_port) ||
                  result.options.connect_port == 0)))
                return fail("--connect needs HOST[:PORT] with port 1..65535");
            result.options.connect_host = host == "localhost" ? "127.0.0.1" : std::string(host);
            if (!platform::Endpoint::from_string(result.options.connect_host,
                                                 result.options.connect_port))
                return fail("--connect needs a numeric IPv4 address or localhost");
        } else if (arg == "--net-impair") {
            if (!has_value || result.options.net_impair)
                return fail("--net-impair may be given once");
            const auto value = args[++i];
            bool delay = false, loss = false, seed = false, jitter = false, blackout = false;
            for (std::size_t start = 0; start < value.size();) {
                const auto comma = value.find(',', start);
                const auto part =
                    value.substr(start, comma == std::string_view::npos ? comma : comma - start);
                if (part.starts_with("delay_ms=") && !delay)
                    delay = parse_uint(part.substr(9), result.options.net_delay_ms);
                else if (part.starts_with("seed=") && !seed)
                    seed = parse_uint(part.substr(5), result.options.net_seed);
                else if (part.starts_with("jitter_ms=") && !jitter)
                    jitter = parse_uint(part.substr(10), result.options.net_jitter_ms);
                else if (part.starts_with("loss=") && !loss) {
                    const auto number = part.substr(5);
                    const auto [end, ec] = std::from_chars(
                        number.data(), number.data() + number.size(), result.options.net_loss);
                    loss = ec == std::errc{} && end == number.data() + number.size() &&
                           std::isfinite(result.options.net_loss) && result.options.net_loss >= 0 &&
                           result.options.net_loss < 1;
                } else if (part.starts_with("blackout_ms=") && !blackout) {
                    const auto at = part.find("@tick=");
                    blackout =
                        at != std::string_view::npos &&
                        parse_uint(part.substr(12, at - 12), result.options.net_blackout_ms) &&
                        parse_uint(part.substr(at + 6), result.options.net_blackout_tick) &&
                        result.options.net_blackout_ms > 0;
                } else {
                    return fail("invalid --net-impair field");
                }
                if (comma == std::string_view::npos)
                    break;
                start = comma + 1;
            }
            if (!delay || !seed || (value.find("loss=") != std::string_view::npos && !loss) ||
                (value.find("jitter_ms=") != std::string_view::npos && !jitter) ||
                (value.find("blackout_ms=") != std::string_view::npos && !blackout))
                return fail("--net-impair needs delay_ms=D,seed=S and valid optional fields");
            result.options.net_impair = true;
        } else if (arg == "--ticks") {
            if (!has_value || !parse_uint(args[++i], result.options.ticks) ||
                result.options.ticks == 0) {
                return fail("--ticks needs a positive integer");
            }
        } else if (arg == "--frames") {
            unsigned frames = 0;
            if (!has_value || !parse_uint(args[++i], frames) || frames == 0 ||
                frames > static_cast<unsigned>(std::numeric_limits<int>::max())) {
                return fail("--frames needs a positive integer");
            }
            result.options.frames = static_cast<int>(frames);
        } else if (arg == "--workers") {
            if (!has_value || !parse_uint(args[++i], result.options.workers)) {
                return fail("--workers needs a non-negative integer");
            }
        } else if (arg == "--content") {
            if (!has_value || args[i + 1].empty()) {
                return fail("--content needs a directory");
            }
            result.options.content_dir = std::string{args[++i]};
        } else if (arg == "--input-trace") {
            if (!has_value || args[i + 1].empty()) {
                return fail("--input-trace needs a path");
            }
            result.options.input_trace = std::string{args[++i]};
        } else if (arg == "--port") {
            if (!has_value || !parse_uint(args[++i], result.options.port)) {
                return fail("--port needs an integer in 0..65535");
            }
            port_given = true;
        } else if (arg == "--listen") {
            if (!has_value) {
                return fail("--listen needs ADDR:PORT");
            }
            const std::string_view address = args[++i];
            if (address.starts_with("[")) {
                const std::size_t close = address.find(']');
                std::uint16_t parsed_port = 0;
                if (close == std::string_view::npos || close == 1 || close + 1 >= address.size() ||
                    address[close + 1] != ':' ||
                    !parse_uint(address.substr(close + 2), parsed_port)) {
                    return fail("--listen needs ADDR:PORT");
                }
                return fail("IPv6 is not supported yet");
            }
            const std::size_t colon = address.rfind(':');
            if (colon == std::string_view::npos || colon == 0 ||
                address.substr(0, colon).find(':') != std::string_view::npos ||
                !parse_uint(address.substr(colon + 1), result.options.port)) {
                return fail("--listen needs ADDR:PORT");
            }
            result.options.listen_host = std::string(address.substr(0, colon));
            if (!platform::Endpoint::from_string(result.options.listen_host, result.options.port)) {
                return fail("--listen needs a numeric IPv4 ADDR:PORT");
            }
            result.options.explicit_listen = true;
        } else if (arg == "--offline") {
            result.options.offline = true;
        } else {
            return fail(fmt::format("unknown argument '{}'", arg));
        }
    }

    // Combinations that parse but mean nothing. Refused rather than ignored: `dedicated --frames 8`
    // asks for rendered frames from a mode that renders none, and quietly running it as something
    // else would leave the user believing they measured what they asked for.
    if (result.options.mode == LaunchMode::Dedicated && result.options.frames > 0) {
        return fail("dedicated renders no frames; bound it with --ticks instead");
    }
    if (result.options.explicit_listen && port_given) {
        return fail("--listen and --port cannot be combined");
    }
    if (result.options.offline && (result.options.explicit_listen || port_given)) {
        return fail("--offline cannot be combined with --listen or --port");
    }
    if (result.options.mode != LaunchMode::Dedicated &&
        (result.options.offline || result.options.explicit_listen)) {
        return fail("--listen and --offline require dedicated mode");
    }
    if (!result.options.input_trace.empty() &&
        (result.options.mode != LaunchMode::Dedicated || result.options.offline)) {
        return fail("--input-trace requires listening dedicated mode");
    }
    if (result.options.spawn_fault != SpawnFault::None &&
        (result.options.mode != LaunchMode::Dedicated || result.options.offline))
        return fail("--spawn-fault requires listening dedicated mode");
    {
        const auto fault = result.options.spawn_fault;
        const bool timed = fault == SpawnFault::Duplicate || fault == SpawnFault::LateAlive ||
                           fault == SpawnFault::LateEnded;
        if (timed != (result.options.spawn_fault_ticks != 0))
            return fail("--spawn-fault-ticks goes with duplicate, late-alive or late-ended only");
    }
    if ((result.options.spawn_debug_break != SpawnDebugBreak::None ||
         result.options.spawn_unresolved_bound != 0) &&
        result.options.connect_host.empty())
        return fail("spawn client diagnostics require --connect");
    if (!result.options.connect_host.empty() && result.options.mode != LaunchMode::Play)
        return fail("--connect requires play mode");
    if (!result.options.connect_host.empty() &&
        (result.options.ticks > 0 || port_given || !result.options.content_dir.empty()))
        return fail("connected play does not use --ticks, --port or --content");
    if ((result.options.no_gpu || result.options.no_prediction || result.options.input_tape ||
         result.options.net_impair || result.options.has_snap_bound ||
         result.options.has_profile_version_override || result.options.has_fixed_dt_override ||
         result.options.client_port != 0) &&
        result.options.connect_host.empty())
        return fail("--no-gpu, --no-prediction and --net-impair require --connect");
    if (linger_given && (result.options.mode != LaunchMode::Dedicated || result.options.offline ||
                         result.options.ticks == 0))
        return fail("--linger-ms requires bounded listening dedicated mode");
    return result;
}

RunReport run_game_mode(const LaunchOptions& options,
                        const GameDefinition& definition,
                        const RunHooks& hooks) {
    RunReport report{};
    report.mode = options.mode;
    const bool listening = options.mode == LaunchMode::Dedicated && !options.offline;
    if (!options.connect_host.empty())
        return run_network_client(options, definition, hooks);

    // ── 1. The modes this build cannot run, refused by name before anything is constructed ──────
    switch (options.mode) {
        case LaunchMode::Play:
        case LaunchMode::Dedicated:
            break;
        case LaunchMode::Browser:
            return refuse(report,
                          RunStatus::NotYetImplemented,
                          "'browser' mode is not implemented yet: it needs the rime-gateway "
                          "session spawn (ADR-0045/0046) — this build can run 'play' or "
                          "'dedicated'");
        case LaunchMode::Stream:
            return refuse(report,
                          RunStatus::NotYetImplemented,
                          "'stream' mode is not implemented yet: the capture/encode pipeline "
                          "exists (samples/04-remote-view) but is not wired to a game — this "
                          "build can run 'play' or 'dedicated'");
        case LaunchMode::Host:
            return refuse(report,
                          RunStatus::NotYetImplemented,
                          "'host' mode is not implemented yet: listening for, admitting and "
                          "replicating to remote players is later M20 work — this build can run "
                          "'play' or 'dedicated'");
    }
    if (!definition.create) {
        return refuse(report, RunStatus::Failed, "the GameDefinition has no create() factory");
    }
    if (listening && (!definition.network_input || !definition.network_input->to_actions ||
                      definition.network_input->profile_id.empty())) {
        report.usage_error = true;
        return refuse(
            report, RunStatus::Failed, "this game has no network input profile; use --offline");
    }
    if (!options.input_trace.empty() && !listening) {
        report.usage_error = true;
        return refuse(report, RunStatus::Failed, "--input-trace requires listening dedicated mode");
    }

    // ── 1b. The content root, before anything is constructed (m20.2) ─────────────────────────────
    //
    // Resolved here, ahead of the Application, for the same reason the stub modes refuse here: a
    // run that cannot find its content must say so before it has opened a window or a device, and
    // say WHERE it looked — a missing root is never a silently empty world. A game declares content
    // by naming a relative entry scene or a dev content directory; one that does neither (it builds
    // its world in code from nothing) searches nothing.
    std::filesystem::path entry_scene_path;
    const std::filesystem::path entry{definition.entry_scene};
    const bool declares_content =
        (!entry.empty() && entry.is_relative()) || !definition.dev_content_dir.empty();
    if (declares_content) {
        ContentSearch search{};
        search.explicit_dir = options.content_dir;
        search.dev_content_dir = definition.dev_content_dir;
        search.dev_binary_dir = definition.dev_binary_dir;
        search.required = entry.is_relative() ? entry : std::filesystem::path{};
        report.content = resolve_content_root(search);
        if (!report.content.ok) {
            return refuse(report, RunStatus::Failed, describe_content_failure(report.content));
        }
        if (hooks.log_content_root) {
            fmt::print(listening ? stderr : stdout,
                       "{}: content root {} ({})\n",
                       definition.name,
                       report.content.dir.string(),
                       report.content.why);
        }
        if (!entry.empty()) {
            entry_scene_path = entry.is_relative() ? report.content.dir / entry : entry;
        }
    } else if (!entry.empty()) {
        entry_scene_path = entry; // absolute: a test's or a tool's file, loaded as given
    }

    // ── 2. THE DECISION: may this run have a device? Made here, before an Application exists ────
    //
    // The counting wrapper is installed in EVERY mode, dedicated included, and that is deliberate:
    // it is `gpu` that keeps dedicated away from the factory, and the counter is what proves it. A
    // wrapper installed only in the device modes could not tell "dedicated never asked" from
    // "dedicated asked something we were not counting".
    const bool wants_device = mode_uses_device(options.mode);
    AppConfig config{};
    config.tick_hz = definition.tick_hz;
    config.worker_threads = options.workers;
    config.gpu = wants_device;
    config.render_extent = kPlayRenderExtent;
    config.windowed = wants_device && !options.headless;
    config.window_title = definition.name;
    config.window_size = kPlayRenderExtent;
    std::uint64_t factory_calls = 0;
    config.device_factory = [&factory_calls, &hooks]() -> std::unique_ptr<rhi::Device> {
        ++factory_calls;
        return hooks.device_factory ? hooks.device_factory() : rhi::create_device({});
    };

    Application app(config);
    report.device_factory_calls = factory_calls;
    report.had_device = app.device() != nullptr;
    report.windowed = app.windowed();

    // ── 3. The game's world: components, entry scene, then the game's own setup ─────────────────
    if (definition.register_components) {
        definition.register_components(app.world());
    }
    if (definition.remote_play && definition.remote_play->register_replicated) {
        definition.remote_play->register_replicated(app.world());
    }
    if (!entry_scene_path.empty()) {
        const scene::LoadReport loaded = scene::load_scene_file(app.world(), entry_scene_path);
        if (!loaded.ok) {
            return refuse(report,
                          RunStatus::Failed,
                          fmt::format("entry scene '{}' did not load: {}",
                                      entry_scene_path.string(),
                                      loaded.error));
        }
        // A load writes LocalTransforms only — WorldTransform is derived state and deliberately not
        // in the file. So give every loaded entity a WorldTransform and compose it, so `setup` (and
        // PhysicsSync, which binds bodies from WorldTransform) sees the world where the scene put
        // it. m20.1 called `propagate_transforms` alone, which only UPDATES entities that already
        // have one: a scene-loaded body had none and would never have been simulated. No game used
        // an entry scene until m20.2's hello-game content, which is what found it.
        scene::derive_world_transforms(app.world(), app.jobs());
    }

    // Created AFTER the Application so it is destroyed BEFORE it: a game owns things (a physics
    // world pointed at `app.jobs()`) that must not outlive the loop they were built against.
    std::unique_ptr<Game> game = definition.create();
    if (!game) {
        return refuse(report, RunStatus::Failed, "the GameDefinition's create() returned null");
    }
    if (options.autopilot && !game->has_autopilot()) {
        return refuse(report,
                      RunStatus::Failed,
                      fmt::format("--autopilot: '{}' ships no autopilot", definition.name));
    }
    SetupContext setup{app.world(), app.jobs(), app.fixed_dt(), report.content.dir};
    if (!game->setup(setup)) {
        return refuse(report, RunStatus::Failed, "the game's setup() refused to start");
    }
    if (listening && definition.remote_play &&
        (!definition.remote_play->register_replicated || !definition.remote_play->owned_player ||
         !definition.remote_play->prediction_hooks || !definition.remote_play->digest ||
         definition.remote_play->fixed_dt != app.fixed_dt())) {
        return refuse(report, RunStatus::Failed, "invalid remote play profile or fixed timestep");
    }

    std::unique_ptr<NetworkInputServer> network;
    std::ofstream input_trace;
    if (listening) {
        if (!options.input_trace.empty()) {
            input_trace.open(options.input_trace, std::ios::out | std::ios::trunc);
            if (!input_trace) {
                return refuse(report, RunStatus::Failed, "could not open input trace");
            }
        }
        network =
            NetworkInputServer::bind(options.listen_host,
                                     options.port,
                                     definition.tick_hz,
                                     *definition.network_input,
                                     app.world(),
                                     definition.remote_play ? &*definition.remote_play : nullptr,
                                     report.bind_error,
                                     options.spawn_fault,
                                     options.spawn_fault_ticks);
        if (!network) {
            report.bind_failed = report.bind_error.starts_with('E');
            return refuse(report,
                          RunStatus::Failed,
                          (report.bind_failed ? "UDP bind failed: " : "remote play refused: ") +
                              report.bind_error);
        }
        fmt::print("{{\"event\":\"ready\",\"transport\":\"udp\",\"bind\":\"{}:{}\",\"codec\":2,"
                   "\"auth\":\"none\"}}\n",
                   options.listen_host,
                   network->local_port());
        std::fflush(stdout);
        fmt::print(stderr,
                   "{}: unauthenticated server reachable on {}:{}\n",
                   definition.name,
                   options.listen_host,
                   network->local_port());
    }

    // ── 4. Presentation — only where a device exists ───────────────────────────────────────────
    //
    // Declared after the game so they are destroyed before it, and after the Application's device
    // so they are destroyed before THAT. A mode that wanted a device and got none (no Vulkan here)
    // still runs the game — the simulation is the game — and says what it skipped.
    std::unique_ptr<render::MeshRegistry> meshes;
    std::unique_ptr<render::MaterialRegistry> materials;
    std::unique_ptr<render::SceneRenderer> renderer;
    if (wants_device && app.device() != nullptr) {
        meshes = std::make_unique<render::MeshRegistry>(*app.device());
        materials = std::make_unique<render::MaterialRegistry>();
        renderer = std::make_unique<render::SceneRenderer>(*app.device(), *meshes, *materials);
        renderer->set_frames_in_flight(app.frames_in_flight());
        PresentSetupContext present{app.world(), *meshes, *materials, *renderer};
        game->present_setup(present);
    } else if (wants_device) {
        ++report.presentation_skipped;
    }

    // ── 5. Input ────────────────────────────────────────────────────────────────────────────────
    //
    // Exactly one source per tick. A listening dedicated run first pumps its admitted client;
    // without one, the schedule supplies a neutral stand-in for that tick.
    InputMapper mapper(definition.input);
    report.rejected_bindings = mapper.rejected_bindings();
    const bool keyboard = options.mode == LaunchMode::Play && !options.autopilot;
    // Map each frame's events exactly once, at the first place that frame is seen — its first tick
    // if it runs one, otherwise its render. Mapping in the tick is what removes the one-frame input
    // lag a render-callback-only mapping has (hello-game documented it; the engine now owns input,
    // so the engine fixes it). Mapping in the render as well is what keeps a zero-tick frame's
    // keystrokes from being dropped when the next frame replaces the snapshot.
    std::uint64_t mapped_frame = std::numeric_limits<std::uint64_t>::max();
    auto map_frame_input = [&] {
        if (keyboard && mapped_frame != app.frame_index()) {
            mapper.update(app.frame_input());
            mapped_frame = app.frame_index();
        }
    };

    // ── 6. The tick — the same body in every mode ──────────────────────────────────────────────
    bool game_finished = false;
    std::uint64_t game_ticks = 0;
    std::vector<SpawnReport> spawn_reports;
    app.on_fixed_tick([&](ecs::World& world, double dt) {
        if (game_finished) {
            // A real-clock frame can owe several ticks and the game can finish on the first; the
            // rest are not the game's to run. Counted, never silently eaten.
            ++report.ticks_skipped_after_finish;
            return;
        }
        map_frame_input();
        TickContext tick{world, dt, game_ticks, {}};
        tick.input = options.autopilot ? game->autopilot()
                     : keyboard        ? mapper.take()
                                       : ActionState{};
        if (network) {
            tick.input = network->before_tick(game_ticks);
            tick.command = network->command_provenance();
            tick.refuse_spawn = network->refuse_spawn();
            // The report list lives through publication, so a keyed launch and its first
            // checkpoint describe the same tick rather than disagreeing for one frame.
            tick.spawns = definition.remote_play && definition.remote_play->spawn_prediction
                              ? &spawn_reports
                              : nullptr;
            if (input_trace) {
                // One line per committed tick: tick, applied(1)/synthesized(0), four axes,
                // held and pressed. max_digits10 preserves the exact float for replay.
                input_trace << game_ticks << ' ' << (network->last_applied() ? 1 : 0);
                for (float axis : tick.input.axes) {
                    input_trace << ' '
                                << std::setprecision(std::numeric_limits<float>::max_digits10)
                                << axis;
                }
                input_trace << ' ' << tick.input.held << ' ' << tick.input.pressed << '\n';
            }
        }
        game->fixed_tick(tick);
        if (network) {
            network->after_tick(game_ticks, spawn_reports);
            spawn_reports.clear();
        }
        ++game_ticks;
        if (game->finished()) {
            game_finished = true;
            app.request_quit();
        }
    });

    // ── 7. The frame — installed only when there is a device to present with ───────────────────
    //
    // `dedicated` installs NO render callback at all, rather than one that returns early: the
    // loop's "pure-sim app declares no render frame" path is then the path it takes, and there is
    // no presentation code on its frame to reason about.
    render::RGTexture last_frame{};
    if (wants_device) {
        app.on_render([&](FrameContext& ctx) {
            map_frame_input();
            if (!renderer || ctx.graph == nullptr) {
                ++report.presentation_skipped;
                return;
            }
            PresentFrameContext frame{ctx.world, ctx.frame_dt, ctx.alpha};
            game->present_frame(frame);
            last_frame = renderer->render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
            ctx.present = last_frame;
        });
    }

    // ── 8. Drive it ─────────────────────────────────────────────────────────────────────────────
    if (listening) {
        // A bounded listener still waits for real tick deadlines: racing a 30-tick test through
        // setup would give a client no opportunity to send even one command.
        const auto period = std::chrono::duration<double>(app.fixed_dt());
        auto deadline = std::chrono::steady_clock::now();
        while (!app.quit_requested() && (options.ticks == 0 || game_ticks < options.ticks)) {
            deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            std::this_thread::sleep_until(deadline);
            (void)app.step(app.fixed_dt());
        }
        if (options.linger_ms > 0 && game_ticks > 0)
            network->linger(game_ticks - 1, options.linger_ms);
    } else if (options.ticks > 0) {
        // Bounded and deterministic: a frame dt equal to the fixed dt is exactly one tick per step,
        // with no wall clock anywhere — which is what makes two runs (two modes, two thread counts)
        // comparable tick for tick.
        const double fd = app.fixed_dt();
        for (std::uint64_t i = 0; i < options.ticks && !app.quit_requested(); ++i) {
            (void)app.step(fd);
        }
        app.finish_gpu(); // step() cannot idle the GPU itself; the caller of step() owns it
    } else if (options.frames > 0) {
        app.run_frames(options.frames);
    } else if (options.mode == LaunchMode::Dedicated) {
        // A real-time server loop, PACED: sleep out the rest of each tick. `Application::run()`
        // spins, which a desktop game with a vsync'd window gets away with and a server sharing a
        // box with other servers must not — a dedicated process burning a whole core while idle is
        // the first thing an operator would notice.
        const auto tick_ns = static_cast<std::uint64_t>(app.fixed_dt() * 1e9);
        std::uint64_t last = platform::Clock::now_ns();
        while (!app.quit_requested()) {
            const std::uint64_t now = platform::Clock::now_ns();
            (void)app.step(static_cast<double>(now - last) * 1e-9);
            last = now;
            const std::uint64_t spent = platform::Clock::now_ns() - now;
            if (spent < tick_ns) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(tick_ns - spent));
            }
        }
    } else {
        (void)app.run();
    }

    report.ticks = game_ticks;
    report.frames = app.frame_index();
    report.finished = game_finished;
    report.windowed = app.windowed();
    report.state_digest = definition.remote_play && definition.remote_play->digest
                              ? definition.remote_play->digest(app.world())
                              : game->state_digest(app.world());
    if (network) {
        report.network_input = network->counters();
        if (options.ticks > 0) {
            // Keep the m20.4 event byte-identical for games without the profile. An opted-in
            // server names every refusal and skip beside the existing input counters.
            std::string spawn_counters =
                definition.remote_play && definition.remote_play->spawn_prediction
                    ? fmt::format(",\"spawn_accepted\":{},\"spawn_rejected\":{},"
                                  "\"spawn_accepted_ended\":{},\"spawn_epoch_advances\":{},"
                                  "\"spawn_outcomes_deferred\":{},\"spawn_outcomes_dropped\":{},"
                                  "\"spawn_foreign_key\":{},\"spawn_out_of_order\":{},"
                                  "\"spawn_end_unknown\":{},\"spawn_roster_refused\":{},"
                                  "\"spawn_without_client\":{},\"spawn_extension_refused\":{},"
                                  "\"spawn_acks_applied\":{},\"spawn_ack_malformed\":{},"
                                  "\"spawn_ack_wrong_channel\":{},"
                                  "\"spawn_ack_wrong_incarnation\":{},\"spawn_ack_ahead\":{},"
                                  "\"spawn_ack_stale\":{}",
                                  report.network_input.spawn_accepted,
                                  report.network_input.spawn_rejected,
                                  report.network_input.spawn_accepted_ended,
                                  report.network_input.spawn_epoch_advances,
                                  report.network_input.spawn_outcomes_deferred,
                                  report.network_input.spawn_outcomes_dropped,
                                  report.network_input.spawn_foreign_key,
                                  report.network_input.spawn_out_of_order,
                                  report.network_input.spawn_end_unknown,
                                  report.network_input.spawn_roster_refused,
                                  report.network_input.spawn_without_client,
                                  report.network_input.spawn_extension_refused,
                                  report.network_input.spawn_acks_applied,
                                  report.network_input.spawn_ack_malformed,
                                  report.network_input.spawn_ack_wrong_channel,
                                  report.network_input.spawn_ack_wrong_incarnation,
                                  report.network_input.spawn_ack_ahead,
                                  report.network_input.spawn_ack_stale)
                    : std::string{};
            if (definition.remote_play && definition.remote_play->spawn_prediction)
                spawn_counters += fmt::format(
                    ",\"spawn_fault_refuse\":{},\"spawn_fault_lose_one\":{},"
                    "\"spawn_fault_lose_all\":{},\"spawn_fault_duplicate\":{},"
                    "\"spawn_fault_late_alive\":{},\"spawn_fault_late_ended\":{},"
                    "\"launch_recovered_by_redundancy\":{},\"spawn_last_flight_ticks\":{}",
                    report.network_input.spawn_fault_refuse,
                    report.network_input.spawn_fault_lose_one,
                    report.network_input.spawn_fault_lose_all,
                    report.network_input.spawn_fault_duplicate,
                    report.network_input.spawn_fault_late_alive,
                    report.network_input.spawn_fault_late_ended,
                    report.network_input.launch_recovered_by_redundancy,
                    report.network_input.spawn_last_flight_ticks);
            if (definition.remote_play && definition.remote_play->active_interaction())
                spawn_counters += fmt::format(
                    ",\"interaction_joins\":{},\"interaction_leaves\":{},"
                    "\"interaction_joined_in_contact\":{},"
                    "\"interaction_leave_deferred_moving\":{},"
                    "\"interaction_leave_deferred_touching\":{},"
                    "\"interaction_epoch_advances\":{},\"interaction_member_ticks\":{},"
                    "\"interaction_capture_refused\":{}",
                    report.network_input.interaction_joins,
                    report.network_input.interaction_leaves,
                    report.network_input.interaction_joined_in_contact,
                    report.network_input.interaction_leave_deferred_moving,
                    report.network_input.interaction_leave_deferred_touching,
                    report.network_input.interaction_epoch_advances,
                    report.network_input.interaction_member_ticks,
                    report.network_input.interaction_capture_refused);
            fmt::print("{{\"event\":\"finished\",\"ticks\":{},\"digest\":\"0x{:016x}\","
                       "\"applied\":{},\"synthesized\":{},\"neutralized\":{},"
                       "\"late\":{},\"duplicate\":{},\"future\":{},"
                       "\"truncated\":{},\"bad_length\":{},\"bad_count\":{},"
                       "\"unknown_codec\":{},\"reserved_nonzero\":{},\"non_finite\":{},"
                       "\"sequence_order\":{},\"hello_malformed\":{},\"hello_version\":{},"
                       "\"hello_schema\":{},\"unadmitted_messages\":{},\"wrong_channel\":{},"
                       "\"oversized\":{},\"clients_admitted\":{},\"clients_refused\":{},"
                       "\"clients_left\":{},\"refusal_close_deferred\":{},"
                       "\"amplification_blocked\":{},\"driver_dropped\":{},"
                       "\"replication_acks\":{},\"replication_ack_refused\":{},"
                       "\"replication_records_too_large\":{},"
                       "\"replication_budget_deferred\":{},\"replication_orphans\":{},"
                       "\"replication_clients_released\":{},"
                       "\"checkpoint_encoded\":{},\"checkpoint_encode_refused\":{},"
                       "\"checkpoint_send_refused\":{},\"input_result_send_refused\":{},"
                       "\"replication_registration_refused\":{},\"publish_missing_session\":{},"
                       "\"checkpoint_capture_refused\":{}{}}}\n",
                       report.ticks,
                       report.state_digest,
                       report.network_input.applied,
                       report.network_input.synthesized,
                       report.network_input.neutralized,
                       report.network_input.late,
                       report.network_input.duplicate,
                       report.network_input.future,
                       report.network_input.truncated,
                       report.network_input.bad_length,
                       report.network_input.bad_count,
                       report.network_input.unknown_codec,
                       report.network_input.reserved_nonzero,
                       report.network_input.non_finite,
                       report.network_input.sequence_order,
                       report.network_input.hello_malformed,
                       report.network_input.hello_version,
                       report.network_input.hello_schema,
                       report.network_input.unadmitted_messages,
                       report.network_input.wrong_channel,
                       report.network_input.oversized,
                       report.network_input.clients_admitted,
                       report.network_input.clients_refused,
                       report.network_input.clients_left,
                       report.network_input.refusal_close_deferred,
                       report.network_input.amplification_blocked,
                       report.network_input.driver_dropped,
                       report.network_input.replication_acks,
                       report.network_input.replication_ack_refused,
                       report.network_input.replication_records_too_large,
                       report.network_input.replication_budget_deferred,
                       report.network_input.replication_orphans,
                       report.network_input.replication_clients_released,
                       report.network_input.checkpoint_encoded,
                       report.network_input.checkpoint_encode_refused,
                       report.network_input.checkpoint_send_refused,
                       report.network_input.input_result_send_refused,
                       report.network_input.replication_registration_refused,
                       report.network_input.publish_missing_session,
                       report.network_input.checkpoint_capture_refused,
                       spawn_counters);
            std::fflush(stdout);
        }
    }
    if (hooks.on_finished) {
        hooks.on_finished(app, *game, last_frame);
    }
    return report;
}

int run_game(int argc, char** argv, const GameDefinition& definition) {
    std::vector<std::string_view> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const std::string_view program = argc > 0 ? std::string_view{argv[0]} : definition.name;

    const ParseResult parsed = parse_launch(args);
    if (parsed.status == ParseStatus::Help) {
        fmt::print("{}", launch_usage(program));
        return kExitOk;
    }
    if (parsed.status == ParseStatus::Error) {
        fmt::print(stderr, "{}: {}\n{}", definition.name, parsed.error, launch_usage(program));
        return kExitUsage;
    }

    const RunReport r = run_game_mode(parsed.options, definition);
    switch (r.status) {
        case RunStatus::NotYetImplemented:
            fmt::print(stderr, "{}: {}\n", definition.name, r.message);
            return kExitNotYet;
        case RunStatus::Failed:
            if (r.bind_failed) {
                fmt::print("{{\"event\":\"bind_failed\",\"bind\":\"{}:{}\",\"error\":\"{}\"}}\n",
                           parsed.options.listen_host,
                           parsed.options.port,
                           r.bind_error);
                std::fflush(stdout);
            }
            fmt::print(stderr, "{}: {}\n", definition.name, r.message);
            return r.usage_error ? kExitUsage : kExitFailed;
        case RunStatus::Ok:
            break;
    }
    // One line an operator can grep and a script can compare. The digest is printed in every mode
    // so "does my dedicated server agree with my client" is answerable from two log lines.
    fmt::print(r.mode == LaunchMode::Dedicated && !parsed.options.offline ? stderr : stdout,
               "{}: {} — {} ticks, {} frames, device {}, digest {:016x}{}\n",
               definition.name,
               to_string(r.mode),
               r.ticks,
               r.frames,
               r.had_device ? "yes" : (r.device_factory_calls == 0 ? "never requested" : "none"),
               r.state_digest,
               r.finished ? ", finished" : "");
    return kExitOk;
}

} // namespace rime::app
