// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine-owned `main` of a shipped game (m20.1, ADR-0046 §1, ADR-0056-m20.1): parse the mode,
// decide whether a device may exist, then run the ONE loop every mode shares around the game's
// `Game` object. The header states what is real and what is a stub; this file is written so the
// ordering that makes `dedicated` GPU-free can be read top to bottom in `run_game_mode`.

#include "rime/app/launch.hpp"

#include <fmt/core.h>

#include <charconv>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/platform/clock.hpp"
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
        "  dedicated   a headless server: no window, no GPU device, no Vulkan loaded\n"
        "  browser     host for a browser          (not yet implemented)\n"
        "  stream      a CLI streaming server      (not yet implemented)\n"
        "  host        host a multiplayer session  (not yet implemented)\n"
        "options:\n"
        "  --ticks N     run exactly N fixed ticks, deterministically, then stop\n"
        "  --frames N    play: run N real-clock frames, then stop\n"
        "  --autopilot   drive the game from its built-in autopilot\n"
        "  --headless    play: render off-screen instead of opening a window\n"
        "  --workers N   job-system worker threads (0 = one per core, less one)\n"
        "  --port N      the port a listening mode binds\n"
        "  --content DIR the game's content root (default: content/ beside this executable)\n",
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
        } else if (arg == "--port") {
            if (!has_value || !parse_uint(args[++i], result.options.port) ||
                result.options.port == 0) {
                return fail("--port needs an integer in 1..65535");
            }
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
    return result;
}

RunReport run_game_mode(const LaunchOptions& options,
                        const GameDefinition& definition,
                        const RunHooks& hooks) {
    RunReport report{};
    report.mode = options.mode;

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
            fmt::print("{}: content root {} ({})\n",
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
    // Exactly one source per run. The keyboard exists only in `play`; `dedicated` has no local
    // player, so without --autopilot its game receives an empty ActionState every tick — which is
    // what a server with no clients connected is. (Network input is the host/dedicated brick.)
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
        game->fixed_tick(tick);
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
    if (options.ticks > 0) {
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
    report.state_digest = game->state_digest(app.world());
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
            fmt::print(stderr, "{}: {}\n", definition.name, r.message);
            return kExitFailed;
        case RunStatus::Ok:
            break;
    }
    // One line an operator can grep and a script can compare. The digest is printed in every mode
    // so "does my dedicated server agree with my client" is answerable from two log lines.
    fmt::print("{}: {} — {} ticks, {} frames, device {}, digest {:016x}{}\n",
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
