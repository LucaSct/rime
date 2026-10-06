// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// hello-game — the smallest thing in this repository that is a *game* rather than a demonstration.
//
// Walk a character around a floor, touch five markers, push a crate out of the way, win. That is
// the whole design, and the smallness is the feature: two milestones point at this sample for two
// different reasons and both of them want it small.
//
//   * ADR-0038 §"what's missing" named it as **m15.7, the on-ramp** — the sample a newcomer reads
//     first. Every other sample here demonstrates a subsystem (a triangle, a render graph, a
//     physics scene); none of them shows the shape of a game.
//   * ADR-0046 §1 then made it a **prerequisite of M20**, the shipped game: its own "riskiest
//     assumption" is that a game's play loop can be lifted out of its sample `main` into an
//     engine-owned `GameDefinition` without rewriting the game. m20.1 did that lift, here.
//
// **So there are two files, and the split is the point.** `game.hpp/.cpp` is everything the game
// IS — its world, its rules, its autopilot, what it looks like — as a `rime::app::Game`, handed to
// the engine as a `rime::app::GameDefinition`. This file is everything about RUNNING it that is not
// the engine's job: this sample's own self-checks. The loop, the device, the window, the key
// mapping and the launch mode are the engine's (`rime/app/launch.hpp`), and a shipped game's whole
// `main` is `return rime::app::run_game(argc, argv, hello_game::definition());` — which is exactly
// what the mode words below do.
//
//   build/<preset>/bin/hello_game                    # the self-check: GPU-free, silent, exit 0
//   build/<preset>/bin/hello_game --verbose          # the same, with a report
//   build/<preset>/bin/hello_game --headless         # render it off-screen (lavapipe / CI)
//   build/<preset>/bin/hello_game --windowed         # play it: WASD to move, Esc to quit
//   build/<preset>/bin/hello_game --dedicated-proof  # dedicated == play, and no Vulkan in sight
//   build/<preset>/bin/hello_game play|dedicated …   # the shipped-game CLI (`--help` for it)
//   build/<preset>/bin/hello_game --emit-content samples/hello-game/content   # rewrite the arena

#include <fmt/core.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game.hpp"
#include "rime/app/application.hpp"
#include "rime/app/content_root.hpp"
#include "rime/app/launch.hpp"
#include "rime/rhi/device.hpp"
#include "rime/scene/derive_transforms.hpp"
#include "rime/scene/scene_format.hpp"

namespace {

using namespace rime;
using hello_game::HelloGame;

// ── What a run leaves behind ──────────────────────────────────────────────────────────────────
//
// Copied out of the game in `RunHooks::on_finished`, because the engine owns the game's lifetime
// and destroys it when the run returns — a host that wants to know how it went must ask before.
struct RunResult {
    bool won = false;
    int score = 0;
    std::uint64_t ticks = 0;
    std::array<std::uint64_t, HelloGame::kPickupCount> collected_on{};
    std::uint64_t digest = 0;
    std::uint64_t presentation_calls = 0;
};

RunResult result_of(const HelloGame& game, const ecs::World& world) {
    RunResult r{};
    r.won = game.won();
    r.score = game.score();
    r.ticks = game.ticks();
    for (int i = 0; i < HelloGame::kPickupCount; ++i) {
        r.collected_on[static_cast<std::size_t>(i)] =
            game.collected_on(static_cast<std::size_t>(i));
    }
    r.digest = game.state_digest(world);
    r.presentation_calls = game.presentation_calls();
    return r;
}

struct EngineRun {
    app::RunReport report;
    RunResult result;
};

// Run the game THROUGH THE ENGINE in `mode`, bounded to `ticks` deterministic ticks and driven by
// its autopilot. `factory` replaces the device factory for the play-mode legs of the proof.
EngineRun run_through_engine(app::LaunchMode mode,
                             std::uint64_t ticks,
                             unsigned workers,
                             std::function<std::unique_ptr<rhi::Device>()> factory = {}) {
    app::LaunchOptions options{};
    options.mode = mode;
    options.ticks = ticks;
    options.workers = workers;
    options.autopilot = true;
    options.headless = true;
    EngineRun run{};
    app::RunHooks hooks{};
    hooks.device_factory = std::move(factory);
    hooks.log_content_root = false; // this runs the game many times; the self-checks stay quiet
    hooks.on_finished = [&run](app::Application& application, app::Game& game, render::RGTexture) {
        run.result = result_of(static_cast<const HelloGame&>(game), application.world());
    };
    run.report = app::run_game_mode(options, hello_game::definition(), hooks);
    return run;
}

// ── The self-check ────────────────────────────────────────────────────────────────────────────

int run_selftest(bool verbose) {
    // ~33 s of game time at 60 Hz; the autopilot needs a few hundred. A bound, so a rule that never
    // completes fails as a timeout rather than hanging CI.
    constexpr std::uint64_t kBudget = 2000;

    // Through the engine's DEDICATED mode since m20.1: this check has always been GPU-free, and
    // `dedicated` is now the engine's name for "the game with no device". Same autopilot, same
    // bound, same numbers — which is the "no behaviour change" half of the port, checked by the
    // collection ticks below matching what the pre-port binary printed.
    const EngineRun first = run_through_engine(app::LaunchMode::Dedicated, kBudget, 0);
    if (first.report.status != app::RunStatus::Ok || !first.result.won) {
        fmt::print(stderr,
                   "hello-game: FAILED — the autopilot collected {} of {} in {} ticks{}{}\n",
                   first.result.score,
                   HelloGame::kPickupCount,
                   first.result.ticks,
                   first.report.message.empty() ? "" : ": ",
                   first.report.message);
        return 1;
    }

    // Determinism, the same property every other sim proof in this repository asserts (ADR-0026):
    // the identical scenario on a DIFFERENT worker-thread count must reach the identical result.
    // Same-thread-count repetition would pass on a racy tick; a different count is what makes the
    // claim mean anything.
    const EngineRun second = run_through_engine(app::LaunchMode::Dedicated, kBudget, 1);
    if (second.result.ticks != first.result.ticks || second.result.score != first.result.score) {
        fmt::print(stderr,
                   "hello-game: FAILED — nondeterministic: {} ticks / {} score vs {} / {}\n",
                   second.result.ticks,
                   second.result.score,
                   first.result.ticks,
                   first.result.score);
        return 1;
    }
    for (std::size_t i = 0; i < first.result.collected_on.size(); ++i) {
        if (second.result.collected_on[i] != first.result.collected_on[i]) {
            fmt::print(stderr,
                       "hello-game: FAILED — marker {} collected on tick {} vs {}\n",
                       i,
                       second.result.collected_on[i],
                       first.result.collected_on[i]);
            return 1;
        }
    }
    if (second.result.digest != first.result.digest) {
        fmt::print(stderr,
                   "hello-game: FAILED — state digest {:016x} on 1 worker vs {:016x}\n",
                   second.result.digest,
                   first.result.digest);
        return 1;
    }

    if (verbose) {
        fmt::print("hello-game self-check\n");
        fmt::print("  markers collected : {} of {}\n", first.result.score, HelloGame::kPickupCount);
        fmt::print("  ticks to win      : {}\n", first.result.ticks);
        fmt::print("  collection ticks  : ");
        for (std::size_t i = 0; i < first.result.collected_on.size(); ++i) {
            fmt::print("{}{}", i == 0 ? "" : ", ", first.result.collected_on[i]);
        }
        fmt::print("\n  state digest      : {:016x}\n", first.result.digest);
        fmt::print("  determinism       : identical on 1 worker and on {}\n",
                   "hardware_concurrency-1");
        fmt::print("  PASS\n");
    }
    return 0;
}

// ── The dedicated proof (m20.1) ───────────────────────────────────────────────────────────────
//
// The in-tree reference: the loop this sample ran BEFORE the engine owned it — an Application with
// no GPU, the game driven by hand from an `on_fixed_tick`, one tick per `step(fixed_dt)`. It uses
// none of `run_game_mode`, which is what makes it a reference for the runner rather than a second
// copy of it.
//
// Since m20.2 the arena is content, so the reference loads it too — by hand, through the same
// content-root search the runner uses (so it reads the SAME file), but with the scene load written
// out here rather than borrowed from `run_game_mode`.
RunResult reference_run(std::uint64_t ticks, unsigned workers) {
    app::AppConfig config{};
    config.worker_threads = workers;
    app::Application application(config);
    const app::GameDefinition def = hello_game::definition();
    app::ContentSearch search{};
    search.dev_content_dir = def.dev_content_dir;
    search.dev_binary_dir = def.dev_binary_dir;
    search.required = hello_game::kEntryScene;
    const app::ContentRoot root = app::resolve_content_root(search);
    if (!root.ok) {
        fmt::print(stderr, "hello-game: {}\n", app::describe_content_failure(root));
        return {};
    }
    hello_game::register_components(application.world());
    if (!scene::load_scene_file(application.world(), root.dir / hello_game::kEntryScene).ok) {
        return {};
    }
    scene::derive_world_transforms(application.world(), application.jobs());
    HelloGame game;
    app::SetupContext setup{
        application.world(), application.jobs(), application.fixed_dt(), root.dir};
    if (!game.setup(setup)) {
        return {};
    }
    std::uint64_t tick = 0;
    application.on_fixed_tick([&](ecs::World& world, double dt) {
        app::TickContext ctx{world, dt, tick++, game.autopilot()};
        game.fixed_tick(ctx);
    });
    const double fd = application.fixed_dt();
    for (std::uint64_t i = 0; i < ticks && !game.finished(); ++i) {
        (void)application.step(fd);
    }
    return result_of(game, application.world());
}

// Is the Vulkan loader mapped into THIS process? On Linux `/proc/self/maps` lists every mapped
// file, so the answer is a fact about the process rather than about the code path we think ran —
// which is the difference between "dedicated does not call the factory" (the counter) and
// "dedicated does not load Vulkan" (this). volk dlopen()s libvulkan inside `create_device` and
// nowhere else, and `ldd hello_game` shows no libvulkan dependency, so a dedicated run that maps
// it has touched the GPU path by some route the counter could not see.
//
// nullopt where there is no /proc (macOS, Windows): the check is then reported as NOT RUN, never as
// passed.
std::optional<bool> vulkan_loader_mapped() {
    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libvulkan") != std::string::npos) {
            return true;
        }
    }
    return false;
}

int run_dedicated_proof(bool verbose) {
    int failures = 0;
    auto check = [&](bool ok, std::string_view what) {
        if (!ok) {
            ++failures;
            fmt::print(stderr, "hello-game: dedicated proof FAILED — {}\n", what);
        } else if (verbose) {
            fmt::print("  ok  {}\n", what);
        }
    };
    auto vulkan_absent = [&](std::string_view when) {
        const std::optional<bool> mapped = vulkan_loader_mapped();
        if (!mapped) {
            fmt::print("  --  libvulkan mapping {}: NOT RUN (no /proc/self/maps here)\n", when);
            return;
        }
        check(!*mapped, fmt::format("libvulkan is not mapped {}", when));
    };

    if (verbose) {
        fmt::print("hello-game dedicated proof\n");
    }
    // The ORDER of this function is part of the proof: every leg that must not load Vulkan runs
    // before the one leg that may, because once a loader is mapped it stays mapped.
    vulkan_absent("before anything ran");

    // Two bounds: 120 ticks stops MID-GAME (the crate is being approached, nothing is won — a
    // digest of a finished game would hide a divergence that the ending happened to erase), and
    // 2000 runs to the win.
    constexpr std::array<std::uint64_t, 2> kBounds{120, 2000};
    std::array<RunResult, kBounds.size()> reference{};
    for (std::size_t b = 0; b < kBounds.size(); ++b) {
        const std::uint64_t n = kBounds[b];
        reference[b] = reference_run(n, 0);
        check(reference[b].digest != 0,
              fmt::format("[{} ticks] the reference run has a digest", n));

        // DEDICATED, with a counting factory handed in: the proof is the count, not the absence of
        // a window.
        std::uint64_t hook_calls = 0;
        const EngineRun ded = run_through_engine(app::LaunchMode::Dedicated, n, 0, [&hook_calls] {
            ++hook_calls;
            return rhi::create_device({});
        });
        check(ded.report.status == app::RunStatus::Ok,
              fmt::format("[{} ticks] dedicated ran{}{}",
                          n,
                          ded.report.message.empty() ? "" : ": ",
                          ded.report.message));
        check(ded.report.device_factory_calls == 0 && hook_calls == 0,
              fmt::format("[{} ticks] dedicated never entered the device factory ({} / {} calls)",
                          n,
                          ded.report.device_factory_calls,
                          hook_calls));
        check(!ded.report.had_device && !ded.report.windowed,
              fmt::format("[{} ticks] dedicated had no device and no window", n));
        check(ded.result.presentation_calls == 0,
              fmt::format("[{} ticks] dedicated never called a presentation hook ({} calls)",
                          n,
                          ded.result.presentation_calls));
        check(ded.report.frames == ded.report.ticks && ded.report.ticks == reference[b].ticks,
              fmt::format("[{} ticks] one tick per frame, and the reference's tick count "
                          "({} frames / {} ticks / reference {})",
                          n,
                          ded.report.frames,
                          ded.report.ticks,
                          reference[b].ticks));
        check(ded.result.digest == reference[b].digest &&
                  ded.result.collected_on == reference[b].collected_on,
              fmt::format("[{} ticks] dedicated digest {:016x} == in-tree reference {:016x}",
                          n,
                          ded.result.digest,
                          reference[b].digest));
        vulkan_absent(fmt::format("after dedicated ran {} ticks", n));

        // PLAY on a machine with no GPU: the factory is entered (exactly once) and answers null,
        // which is what `rhi::create_device` does with no Vulkan loader. The game still runs — the
        // simulation is the game — and presentation is skipped and counted.
        const EngineRun play_nogpu =
            run_through_engine(app::LaunchMode::Play, n, 0, [] { return nullptr; });
        check(play_nogpu.report.device_factory_calls == 1 && !play_nogpu.report.had_device,
              fmt::format("[{} ticks] play asked for a device exactly once", n));
        check(play_nogpu.report.presentation_skipped > 0 &&
                  play_nogpu.result.presentation_calls == 0,
              fmt::format("[{} ticks] play without a device skipped presentation, and counted it",
                          n));
        check(play_nogpu.result.digest == reference[b].digest,
              fmt::format("[{} ticks] play (no GPU) digest {:016x} == reference {:016x}",
                          n,
                          play_nogpu.result.digest,
                          reference[b].digest));
    }
    vulkan_absent("after every GPU-free leg");

    // PLAY WITH A REAL DEVICE, last, because it maps the loader. The strongest leg: presentation
    // runs — meshes registered, render components ADDED to the simulated entities, a frame drawn
    // every tick — and the simulation must not notice. This is what holds `present_setup` to "must
    // not change what the simulation computes".
    const EngineRun play_gpu = run_through_engine(app::LaunchMode::Play, kBounds[0], 0);
    if (!play_gpu.report.had_device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            check(false, "a Vulkan device (RIME_REQUIRE_VULKAN is set)");
        } else {
            fmt::print("  --  play with a device: SKIPPED (no Vulkan device here)\n");
        }
    } else {
        check(play_gpu.result.presentation_calls > kBounds[0],
              fmt::format("[{} ticks] play presented (setup + {} frames)",
                          kBounds[0],
                          play_gpu.result.presentation_calls));
        check(play_gpu.result.digest == reference[0].digest,
              fmt::format("[{} ticks] play (GPU) digest {:016x} == reference {:016x}",
                          kBounds[0],
                          play_gpu.result.digest,
                          reference[0].digest));
    }

    if (failures == 0 && verbose) {
        fmt::print("  PASS\n");
    }
    return failures == 0 ? 0 : 1;
}

// ── The rendered run ──────────────────────────────────────────────────────────────────────────

// Read an LDR target back to CPU memory (the 06/07 pattern). Only the headless self-check does
// this; it is a full pipeline stall and has no business in a frame anyone is watching.
std::vector<std::uint8_t>
read_rgba8(rhi::Device& device, rhi::TextureHandle tex, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size = bytes;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle rb = device.create_buffer(bd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// Two STRUCTURAL claims about the rendered frame, not a golden image (the house rule: properties
// the scene guarantees, checked with margins, so it survives a driver's rounding).
//
// **This exists because a render smoke test that only checks the exit code passes on a black
// frame.** That is not hypothetical here: the render graph culls passes whose outputs nothing
// consumes, and m18.3d spent real time on a measured pass that had been culled for 119 of 120
// frames while every number still looked plausible. So the check is on the PIXELS.
//
//   1. the scene is lit — a floor and a sun mean most of the frame is not black;
//   2. a marker is visible — the pickups are the only GREEN-DOMINANT thing in the scene (the floor
//      is blue-grey, the crate brown, the player blue), so green-dominant pixels are proof the
//      trigger volumes are actually drawn. A lit floor alone would satisfy claim 1 and say nothing
//      about the game. The discriminator is strict — green must beat both other channels by a wide
//      margin — because the first version of this check asked for "gold" and the crate passed it.
bool frame_shows_the_game(const std::vector<std::uint8_t>& px, bool verbose) {
    std::uint64_t lit = 0;
    std::uint64_t marker = 0;
    const std::size_t n = px.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int r = px[i * 4 + 0];
        const int g = px[i * 4 + 1];
        const int b = px[i * 4 + 2];
        if ((r + g + b) / 3 > 20) {
            ++lit;
        }
        if (g > 110 && g > r + 60 && g > b + 60) {
            ++marker;
        }
    }
    const bool ok = lit > n / 20 && marker > 40;
    if (verbose || !ok) {
        fmt::print("  frame check: {} lit / {} marker of {} px -> {}\n",
                   lit,
                   marker,
                   n,
                   ok ? "ok" : "FAILED");
    }
    return ok;
}

int run_rendered(bool windowed, int frames) {
    app::LaunchOptions options{};
    options.mode = app::LaunchMode::Play;
    options.headless = !windowed;
    // Off-screen there is nobody to press a key, so the autopilot drives it — the same one the
    // self-check uses. One render path, two input sources, which is the whole reason the game takes
    // an ActionState.
    options.autopilot = !windowed;
    options.frames = windowed ? 0 : frames;

    RunResult result{};
    std::optional<bool> frame_ok;
    app::RunHooks hooks{};
    hooks.on_finished = [&](app::Application& application,
                            app::Game& game,
                            render::RGTexture last_frame) {
        result = result_of(static_cast<const HelloGame&>(game), application.world());
        if (windowed || application.device() == nullptr || !last_frame.is_valid()) {
            return;
        }
        // The loop guarantees the GPU is idle when this hook runs, so the last frame's graph
        // handles are still valid and this readback reads a finished image, not one mid-flight.
        const std::vector<std::uint8_t> px = read_rgba8(*application.device(),
                                                        application.graph()->physical(last_frame),
                                                        app::kPlayRenderExtent.width,
                                                        app::kPlayRenderExtent.height);
        frame_ok = frame_shows_the_game(px, true);
    };
    const app::RunReport report = app::run_game_mode(options, hello_game::definition(), hooks);
    if (report.status != app::RunStatus::Ok) {
        fmt::print(stderr, "hello-game: {}\n", report.message);
        return 1;
    }
    if (!report.had_device) {
        // No Vulkan here. An honest skip, not a failure: CI runs this on machines with no device,
        // and RIME_REQUIRE_VULKAN is how a machine that SHOULD have one says so.
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            fmt::print(stderr, "hello-game: no Vulkan device and RIME_REQUIRE_VULKAN is set\n");
            return 1;
        }
        fmt::print("hello-game: no Vulkan device — skipping the rendered run\n");
        return 0;
    }
    if (windowed) {
        fmt::print("hello-game: {} frames, {} of {} markers{}\n",
                   report.frames,
                   result.score,
                   HelloGame::kPickupCount,
                   result.won ? " — you win" : "");
        return 0;
    }
    const bool ok = frame_ok.value_or(false);
    fmt::print("hello-game: rendered {} frames, {} of {} markers collected — {}\n",
               frames,
               result.score,
               HelloGame::kPickupCount,
               ok ? "the game is on screen" : "FAILED the frame check");
    return ok ? 0 : 1;
}

// ── The content generator ─────────────────────────────────────────────────────────────────────

// Write the arena to `<dir>/hello-game.rscene` — how `samples/hello-game/content` was made.
int emit_content(const std::filesystem::path& dir) {
    ecs::World world;
    hello_game::build_arena(world);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path out = dir / hello_game::kEntryScene;
    if (!scene::save_scene_file(world, out)) {
        fmt::print(stderr, "hello-game: could not write {}\n", out.string());
        return 1;
    }
    fmt::print("hello-game: wrote {}\n", out.string());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // A mode word first means the shipped-game CLI: hand the whole command line to the engine.
    // This is the entire `main` a game exported under M20 would have.
    if (argc > 1 && argv[1][0] != '-') {
        return app::run_game(argc, argv, hello_game::definition());
    }

    bool verbose = false;
    bool headless = false;
    bool windowed = false;
    bool dedicated_proof = false;
    int frames = 240;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--windowed") {
            windowed = true;
        } else if (arg == "--dedicated-proof") {
            dedicated_proof = true;
        } else if (arg == "--emit-content" && i + 1 < argc) {
            return emit_content(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else {
            fmt::print(stderr,
                       "usage: hello_game [--verbose] [--headless [--frames N]] [--windowed] "
                       "[--dedicated-proof] [--emit-content DIR]\n"
                       "       hello_game play|dedicated|browser|stream|host [options]  "
                       "(see: hello_game play --help)\n");
            return 2;
        }
    }
    if (dedicated_proof) {
        return run_dedicated_proof(verbose);
    }
    if (windowed || headless) {
        return run_rendered(windowed, frames);
    }
    return run_selftest(verbose);
}
