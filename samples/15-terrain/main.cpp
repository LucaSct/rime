// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// 15-terrain — the m19.8e TRAVEL BUDGET: what streamed, LOD'd terrain costs, and how fast a camera
// can move over it before the ground it wants is not there yet (ADR-0073).
//
// M19's requirement is seamless travel. m19.8a–d built the machinery — residency, LOD, appearance
// bakes — and proved it structurally on lavapipe. Proofs say nothing about milliseconds or about
// how fast you can fly, so this sample measures, over a cooked km-scale world
// (`make_world.sh` → `rime terrain-world`), a scripted straight fly-through at three speeds:
//
//   walk 5 m/s · vehicle 30 m/s · aircraft 150 m/s            (and, with --envelope, faster)
//
// and per speed records:
//   * terrain GPU ms (p50/p99) — every `terrain` pass's timestamps, summed per frame;
//   * CPU ms for the selection (both select_terrain_lod calls) and for begin_frame as a whole;
//   * upload bytes per frame, resident bytes, fallback draws, upload-cap waits.
//
// THE TRAVEL ENVELOPE is the fastest speed at which fallback draws — leaves drawn coarser than the
// selection asked, because the finer tiles were not resident yet — stay under 1% of the leaves
// drawn, for the stated byte budget and upload cap. Below it, detail keeps up with the camera;
// above it, the 8d2 fallback is visibly doing the work.
//
// TIME. The camera advances by speed × 1/60 s per frame and the loop is PACED to 60 Hz of wall
// clock: loads finish on job threads in real time, so an unpaced loop running at 400 Hz would give
// them a sixth of the time a real 60 Hz frame does, and report a pessimistic envelope. Each speed
// starts warm — the camera parked at the start until nothing it wants is missing — so the number
// is about TRAVEL, not about the first second of a cold start.
//
// Usage:
//   samples/15-terrain/make_world.sh                     # once: generate + cook the world
//   terrain_flythrough --perf [--world DIR] [--out r.json] [--baseline b.json] [--frames N]
//                      [--budget-mib M] [--cap-kib K] [--envelope] [--width W --height H]
//
// Like every --perf sample this is NOT a CTest: it reports wall-clock time on one named GPU.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/manifest.hpp"
#include "rime/assets/terrain_world.hpp"
#include "rime/core/diagnostics/perf_report.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/core/diagnostics/work_ledger.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/terrain_builder.hpp"
#include "rime/render/terrain_pass.hpp"
#include "rime/render/terrain_residency.hpp"
#include "rime/rhi/device.hpp"

namespace {

using namespace rime;
namespace fs = std::filesystem;

struct Speed {
    const char* name;
    float metres_per_second;
    float altitude; // above the highest ground in the tile beneath, metres
    float pitch;    // radians, negative = looking down
};

// p-th percentile, nearest rank, of a copy.
double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const std::size_t rank =
        static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size())));
    return v[std::clamp<std::size_t>(rank, 1, v.size()) - 1];
}

struct SpeedResult {
    std::string name;
    float mps = 0.0f;
    core::DurationSamples gpu, select, begin;
    std::vector<double> upload_bytes;
    std::uint64_t draws = 0;          // leaves drawn (after culling)
    std::uint64_t fallback_drawn = 0; // ...of which coarser than the selection asked
    std::uint64_t culled = 0;
    std::uint64_t cap_waits = 0;
    std::uint64_t budget_waits = 0;
    std::uint64_t uploads = 0;
    std::uint64_t peak_budget_bytes = 0;
    std::uint64_t peak_resident_bytes = 0;
    std::uint64_t untimed_frames = 0; // frames whose terrain passes outran the timestamp pool
    std::uint64_t uncovered = 0;
    std::uint64_t placeholder = 0;
    std::uint32_t warm_frames = 0;
    int frames = 0;  // measured: --frames, or fewer if the one-way pass ends first
    int on_pace = 0; // frames whose begin_frame + terrain GPU fit one 60 Hz frame (16.67 ms)
    double max_leaves = 0;

    [[nodiscard]] double pace_pct() const {
        return frames == 0 ? 0.0 : 100.0 * on_pace / static_cast<double>(frames);
    }

    [[nodiscard]] double fallback_pct() const {
        return draws == 0
                   ? 100.0
                   : 100.0 * static_cast<double>(fallback_drawn) / static_cast<double>(draws);
    }
};

struct Setup {
    rhi::Device* device;
    const assets::TerrainWorld* world;
    fs::path world_dir;
    const assets::Manifest* manifest;
    fs::path palette_dir;
    std::uint64_t budget;
    std::uint64_t cap;
    std::uint32_t width, height;
    int frames;
};

// Ground height under (x, z): the cooked max_y of the level-0 tile beneath — the manifest's own
// bound, so the camera never dips into a hill without the sample reading a single heightfield.
float ground(const assets::TerrainWorld& w, float x, float z) {
    const assets::TerrainWorldGrid& g = w.grid();
    const auto tx = static_cast<std::int32_t>(std::floor((x - g.origin.x) / g.pitch_x()));
    const auto tz = static_cast<std::int32_t>(std::floor((z - g.origin.z) / g.pitch_z()));
    const assets::TerrainWorldTile* t = w.find(assets::TerrainTileKey{0, {tx, tz}});
    return t != nullptr ? t->max_y : 0.0f;
}

SpeedResult
run_speed(const Setup& s, const Speed& sp, core::PerfReport* report, const std::string& name) {
    SpeedResult r;
    r.name = name;
    r.mps = sp.metres_per_second;
    core::JobSystem jobs(4);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*s.device);
    render::TerrainLayerBuilder builder(*s.device, server, *s.manifest, s.palette_dir);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 2048;
    cfg.byte_budget = s.budget;
    cfg.upload_cap = s.cap;
    cfg.frustum_cull = true;
    cfg.lod.viewport_height_px = static_cast<float>(s.height);
    cfg.lod.vertical_fov = 1.0471976f;
    cfg.lod.pixel_error = 1.0f;
    cfg.lod.step_margin = std::max(1.0f, sp.metres_per_second / 60.0f);
    render::TerrainResidency residency(
        *s.device, pass, server, *s.world, s.world_dir, &builder, cfg);

    // ONE straight pass along the world's diagonal, never back: a camera that re-flew ground it had
    // already streamed would be measuring a warm cache, not travel (the first version ping-ponged
    // and reported 0 % fallback at 2400 m/s for exactly that reason). So a fast speed gets fewer
    // frames — as many as the diagonal holds — rather than a second look at the same tiles.
    const assets::TerrainWorldGrid& g = s.world->grid();
    const float extent = g.pitch_x(s.world->level_count() - 1); // the root's span
    const float a = g.origin.x + 0.04f * extent;
    const float b = g.origin.x + 0.96f * extent;
    const float span = (b - a) * std::sqrt(2.0f);
    const auto eye_at = [&](double travelled) {
        const float t = static_cast<float>(std::min(travelled / span, 1.0));
        const float x = a + t * (b - a);
        const float z = g.origin.z + (x - g.origin.x); // the diagonal
        const float y = ground(*s.world, x, z) + sp.altitude;
        return std::pair<core::Vec3, float>{{x, y, z}, 1.0f};
    };
    const double step = static_cast<double>(sp.metres_per_second) / 60.0;
    r.frames = std::min(s.frames, static_cast<int>(span / step));

    render::TerrainLight light{};
    light.sun_direction = {-0.4f, -0.8f, -0.3f};
    light.sun_irradiance = 3.0f;
    light.ambient = 0.1f;
    const float aspect = static_cast<float>(s.width) / static_cast<float>(s.height);
    const core::Mat4 proj = core::perspective(1.0471976f, aspect, 0.5f, 20000.0f);

    const auto frame = [&](core::Vec3 eye, float dir, bool measure) {
        server.pump();
        const core::Stopwatch begin_watch;
        residency.begin_frame(eye);
        const double begin_ms = begin_watch.elapsed_ms();
        const float c = std::cos(sp.pitch);
        const core::Vec3 at{
            eye.x + dir * c * 0.7071f, eye.y + std::sin(sp.pitch), eye.z + dir * c * 0.7071f};
        const core::Mat4 vp = proj * core::look_at(eye, at, {0.0f, 1.0f, 0.0f});
        render::RenderGraph graph(*s.device);
        const render::RGTexture hdr =
            graph.create_texture({{s.width, s.height}, render::kHdrFormat, "terrain-hdr"});
        const render::RGTexture depth =
            graph.create_texture({{s.width, s.height}, render::kDepthFormat, "terrain-depth"});
        graph.export_texture(hdr); // or the graph culls every terrain pass as unread
        const render::RGColorAttachment clears[] = {
            {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.4f, 0.55f, 0.8f, 1.0f}}};
        const render::RGDepthAttachment dclear{
            depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
        render::RenderGraph::RasterPassDesc cd{};
        cd.colors = clears;
        cd.depth = &dclear;
        graph.add_raster_pass("clear", cd, [](rhi::CommandBuffer&) {});
        const std::uint64_t draws_before = residency.stats().draws;
        const std::uint64_t fb_before = residency.stats().visible_fallback_draws;
        const std::uint64_t culled_before = residency.stats().culled_draws;
        residency.add(graph, hdr, depth, vp, eye, light);
        auto cmd = s.device->begin_commands();
        graph.execute(*cmd);
        // Submit, wait, read the timings AND release the submission (p1). This used to borrow
        // with `wait_and_borrow` and never release, leaking a fence and a command buffer per
        // frame — on NVIDIA a device fd each, so a Release run under a 1024-fd limit died a few
        // thousand frames in with vkCreateFence -> VK_ERROR_OUT_OF_HOST_MEMORY.
        const std::vector<render::RenderGraph::PassTiming> timings =
            graph.submit_and_time(*s.device, std::move(cmd));
        residency.end_frame_blocking();
        const render::TerrainResidencyStats& st = residency.stats();
        if (!measure) {
            return;
        }
        double terrain_ms = 0.0;
        std::uint32_t timed = 0;
        std::vector<core::PassTiming> passes;
        for (const render::RenderGraph::PassTiming& t : timings) {
            if (t.name.rfind("terrain", 0) == 0) {
                terrain_ms += t.gpu_ms;
                ++timed;
            }
        }
        const std::uint64_t drawn = st.draws - draws_before;
        r.untimed_frames += timed < drawn ? 1u : 0u;
        r.gpu.add(terrain_ms);
        r.select.add(st.select_ms);
        r.begin.add(begin_ms);
        r.on_pace += begin_ms + terrain_ms <= 1000.0 / 60.0 ? 1 : 0;
        r.upload_bytes.push_back(static_cast<double>(st.upload_bytes_this_frame));
        r.draws += drawn;
        r.fallback_drawn += st.visible_fallback_draws - fb_before;
        r.culled += st.culled_draws - culled_before;
        r.max_leaves = std::max(r.max_leaves, static_cast<double>(drawn));
        if (report != nullptr) {
            passes.push_back(core::PassTiming{"terrain", terrain_ms});
            report->observe_frame(static_cast<std::uint64_t>(r.gpu.count()), terrain_ms, passes);
            report->observe(r.name + ".gpu", terrain_ms);
            report->observe(r.name + ".select", st.select_ms);
            report->observe(r.name + ".residency", begin_ms);
        }
    };

    // Warm start: parked until nothing it wants is missing (or 20 s), unmeasured.
    const auto [start, start_dir] = eye_at(0.0);
    for (int f = 0; f < 1200; ++f) {
        frame(start, start_dir, false);
        ++r.warm_frames;
        if (f > 4 && residency.stats().missing_this_frame.total() == 0) {
            break;
        }
    }
    const auto base = residency.stats();
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(16'666'667);
    auto next = Clock::now();
    for (int f = 0; f < r.frames; ++f) {
        const auto [eye, dir] = eye_at(static_cast<double>(f + 1) * step);
        frame(eye, dir, true);
        next += period;
        std::this_thread::sleep_until(next);
    }
    const render::TerrainResidencyStats& st = residency.stats();
    r.cap_waits = st.upload_cap_waits - base.upload_cap_waits;
    r.budget_waits = st.byte_budget_waits - base.byte_budget_waits;
    r.uploads = st.uploads - base.uploads;
    r.peak_budget_bytes = st.peak_budget_bytes;
    r.peak_resident_bytes = st.peak_resident_bytes;
    r.uncovered = st.uncovered_draws - base.uncovered_draws;
    r.placeholder = st.fallback_appearance_draws;
    return r;
}

} // namespace

int main(int argc, char** argv) {
    bool perf = false, envelope = false;
    int frames = 600;
    std::uint32_t width = 1920, height = 1080;
    double budget_mib = 96.0, cap_kib = 256.0;
    std::string world_dir = "build/terrain-perf-world";
    const char* out = nullptr;
    const char* baseline = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--perf") {
            perf = true;
        } else if (a == "--envelope") {
            envelope = true;
        } else if (a == "--frames") {
            frames = std::atoi(next());
        } else if (a == "--warmup") {
            (void)next(); // perf.sh's probe passes it; the warm start here is by state
        } else if (a == "--width") {
            width = static_cast<std::uint32_t>(std::atoi(next()));
        } else if (a == "--height") {
            height = static_cast<std::uint32_t>(std::atoi(next()));
        } else if (a == "--world") {
            world_dir = next();
        } else if (a == "--budget-mib") {
            budget_mib = std::atof(next());
        } else if (a == "--cap-kib") {
            cap_kib = std::atof(next());
        } else if (a == "--out") {
            out = next();
        } else if (a == "--baseline") {
            baseline = next();
        } else {
            std::fprintf(stderr,
                         "usage: terrain_flythrough --perf [--world DIR] [--out f.json] "
                         "[--baseline b.json] [--frames N] [--budget-mib M] [--cap-kib K] "
                         "[--envelope] [--width W] [--height H]\n");
            return 2;
        }
    }
    (void)perf;
    frames = std::max(frames, 32);

    const fs::path dir(world_dir);
    const auto text = platform::read_file(dir / "world" / "terrain_perf.terrainworld");
    const auto mtext = platform::read_file(dir / "palette-manifest.txt");
    if (!text || !mtext) {
        std::fprintf(stderr,
                     "15-terrain: no cooked world in %s — run samples/15-terrain/make_world.sh\n",
                     world_dir.c_str());
        return 1;
    }
    auto world = assets::TerrainWorld::parse(
        std::string(reinterpret_cast<const char*>(text->data()), text->size()));
    auto manifest = assets::Manifest::parse(
        std::string(reinterpret_cast<const char*>(mtext->data()), mtext->size()));
    if (!world || !manifest || !world->validate_levels()) {
        std::fprintf(stderr, "15-terrain: the world or its palette manifest does not parse\n");
        return 1;
    }

    std::unique_ptr<rhi::Device> device = rhi::create_device({});
    if (!device) {
        std::fprintf(stderr, "15-terrain: no Vulkan device — there is nothing to measure\n");
        return 1;
    }
    core::MachineFingerprint fp = core::MachineFingerprint::detect();
    const rhi::AdapterInfo& adapter = device->adapter();
    fp.gpu = adapter.name;
    fp.driver = adapter.driver_name + " " + adapter.driver_info;
    fp.width = width;
    fp.height = height;
    fp.preset = "terrain-flythrough";

    std::size_t tile_count = 0;
    for (std::uint32_t l = 0; l < world->level_count(); ++l) {
        tile_count += world->tiles(l).size();
    }
    const std::uint64_t budget = static_cast<std::uint64_t>(budget_mib * 1024.0 * 1024.0);
    const std::uint64_t cap = static_cast<std::uint64_t>(cap_kib * 1024.0);
    std::printf("15-terrain: %s (%s %s), %ux%u, %zu tiles over %u levels, budget %.0f MiB, "
                "cap %.0f KiB/frame, %d frames per speed at 60 Hz\n",
                fp.gpu.c_str(),
                adapter.driver_name.c_str(),
                adapter.driver_info.c_str(),
                width,
                height,
                tile_count,
                world->level_count(),
                budget_mib,
                cap_kib,
                frames);

    const Setup setup{device.get(),
                      &*world,
                      dir / "world",
                      &*manifest,
                      fs::path(RIME_SPLAT_PALETTE_DIR),
                      budget,
                      cap,
                      width,
                      height,
                      frames};
    std::vector<Speed> speeds = {{"walk", 5.0f, 1.8f, -0.05f},
                                 {"vehicle", 30.0f, 2.5f, -0.05f},
                                 {"aircraft", 150.0f, 150.0f, -0.25f}};
    if (envelope) {
        for (const float v : {300.0f, 600.0f, 1200.0f, 2400.0f, 4800.0f}) {
            speeds.push_back({nullptr, v, 150.0f, -0.25f});
        }
    }

    core::PerfReport report;
    report.set_machine(fp);
    report.set_run(core::RunInfo::detect("15-terrain"));
    core::WorkLedger ledger;
    ledger.set("frames.measured", static_cast<std::uint64_t>(frames));
    std::deque<std::string> names; // the ledger keeps string_views: a deque never moves its strings
    const auto key = [&](const std::string& s) -> std::string_view {
        names.push_back(s);
        return names.back();
    };

    std::printf("\n%-9s %6s %6s %8s %8s %8s %8s %9s %9s %10s %10s %7s %7s %6s %8s %7s\n",
                "speed",
                "m/s",
                "frames",
                "gpu p50",
                "gpu p99",
                "sel p50",
                "sel p99",
                "resid p50",
                "resid p99",
                "up p50 KB",
                "up max KB",
                "draws",
                "fb %",
                "pace %",
                "capwait",
                "culled");
    double envelope_mps = 0.0;
    bool envelope_open = true;
    int status = 0;
    for (const Speed& sp : speeds) {
        const std::string name =
            sp.name != nullptr ? sp.name
                               : "fly" + std::to_string(static_cast<int>(sp.metres_per_second));
        SpeedResult r = run_speed(setup, sp, &report, name);
        // p1: a speed hands back every submission it made. Its residency is gone by now and the
        // run has no swapchain, so anything still live is a leak, and any refusal is an upload or
        // frame that silently did not happen — both fail the run rather than flatter its numbers.
        const rhi::SubmissionCounters sc = setup.device->submission_counters();
        if (sc.live_fences != 0 || sc.live_command_buffers != 0 || sc.in_flight_submissions != 0 ||
            sc.failed_submissions != 0) {
            std::fprintf(stderr,
                         "  %s: submissions leaked or refused — %llu fences, %llu command buffers, "
                         "%llu in flight live; %llu refused\n",
                         name.c_str(),
                         static_cast<unsigned long long>(sc.live_fences),
                         static_cast<unsigned long long>(sc.live_command_buffers),
                         static_cast<unsigned long long>(sc.in_flight_submissions),
                         static_cast<unsigned long long>(sc.failed_submissions));
            status = 1;
        }
        const core::Distribution gpu = r.gpu.summarize();
        const core::Distribution sel = r.select.summarize();
        const core::Distribution beg = r.begin.summarize();
        std::printf("%-9s %6.0f %6d %8.3f %8.3f %8.3f %8.3f %9.3f %9.3f %10.1f %10.1f %7llu %7.2f "
                    "%6.1f %8llu %7llu\n",
                    name.c_str(),
                    static_cast<double>(r.mps),
                    r.frames,
                    gpu.p50_ms,
                    gpu.p99_ms,
                    sel.p50_ms,
                    sel.p99_ms,
                    beg.p50_ms,
                    beg.p99_ms,
                    percentile(r.upload_bytes, 50) / 1024.0,
                    percentile(r.upload_bytes, 100) / 1024.0,
                    static_cast<unsigned long long>(r.draws),
                    r.fallback_pct(),
                    r.pace_pct(),
                    static_cast<unsigned long long>(r.cap_waits),
                    static_cast<unsigned long long>(r.culled));
        // THE ENVELOPE needs both: detail kept up (fallback < 1 %), AND the loop kept its 60 Hz
        // pace on ≥ 95 % of frames — a loop that fell behind gave the streamer more wall time
        // per metre than a real 60 Hz frame does, so its fallback number flatters it.
        if (envelope_open && r.fallback_pct() < 1.0 && r.pace_pct() >= 95.0) {
            envelope_mps = r.mps;
        } else {
            envelope_open = false;
        }
        const std::string p = "terrain." + name;
        ledger.set(key(p + ".draws"), r.draws);
        ledger.set(key(p + ".fallback_drawn"), r.fallback_drawn);
        ledger.set(key(p + ".culled"), r.culled);
        ledger.set(key(p + ".uploads"), r.uploads);
        ledger.set(key(p + ".upload_bytes_p50"),
                   static_cast<std::uint64_t>(percentile(r.upload_bytes, 50)));
        ledger.set(key(p + ".upload_bytes_p99"),
                   static_cast<std::uint64_t>(percentile(r.upload_bytes, 99)));
        ledger.set(key(p + ".upload_bytes_max"),
                   static_cast<std::uint64_t>(percentile(r.upload_bytes, 100)));
        ledger.set(key(p + ".peak_budget_bytes"), r.peak_budget_bytes);
        ledger.set(key(p + ".peak_resident_bytes"), r.peak_resident_bytes);
        ledger.set(key(p + ".cap_waits"), r.cap_waits);
        ledger.set(key(p + ".budget_waits"), r.budget_waits);
        ledger.set(key(p + ".uncovered"), r.uncovered);
        ledger.set(key(p + ".placeholder_draws"), r.placeholder);
        ledger.set(key(p + ".untimed_frames"), r.untimed_frames);
        ledger.set(key(p + ".warm_frames"), r.warm_frames);
        ledger.set(key(p + ".frames"), static_cast<std::uint64_t>(r.frames));
        ledger.set(key(p + ".frames_on_pace"), static_cast<std::uint64_t>(r.on_pace));
        ledger.set(key(p + ".max_leaves"), static_cast<std::uint64_t>(r.max_leaves));
        if (r.untimed_frames != 0) {
            std::fprintf(stderr,
                         "  %s: %llu frames drew more terrain passes than the timestamp pool "
                         "times — their GPU column is a lower bound\n",
                         name.c_str(),
                         static_cast<unsigned long long>(r.untimed_frames));
            status = 1;
        }
        if (r.peak_budget_bytes > budget) {
            std::fprintf(stderr, "  %s: the byte budget was exceeded\n", name.c_str());
            status = 1;
        }
    }
    std::printf(
        "\ntravel envelope (fallback < 1%% of drawn leaves and >= 95%% of frames on a 60 Hz "
        "pace, budget %.0f MiB, cap %.0f "
        "KiB/frame): %s%.0f m/s\n",
        budget_mib,
        cap_kib,
        envelope_open && envelope ? ">= " : "",
        envelope_mps);
    ledger.set("terrain.envelope_mps", static_cast<std::uint64_t>(envelope_mps));
    report.set_ledger(ledger);

    core::WorkBudget wb;
    wb.at_least("terrain.walk.draws", 1).at_least("frames.measured", 32);
    for (const core::BudgetViolation& v : wb.check(ledger)) {
        std::fprintf(stderr,
                     "  WORK BUDGET: %.*s = %llu (needs at least %llu)\n",
                     static_cast<int>(v.name.size()),
                     v.name.data(),
                     static_cast<unsigned long long>(v.value),
                     static_cast<unsigned long long>(v.limit));
        status = 1;
    }
    core::PerfGate gate;
    gate.require_samples("frame", 32).max_regression(0.10);
    core::PerfReport loaded;
    const core::PerfReport* baseline_ptr = nullptr;
    if (baseline != nullptr) {
        std::string error;
        if (core::PerfReport::load_file(baseline, loaded, error)) {
            baseline_ptr = &loaded;
        } else {
            std::printf("  (no usable baseline: %s)\n", error.c_str());
        }
    }
    const core::PerfGate::Result result = gate.check(report, baseline_ptr);
    if (!result.ok()) {
        status = 1;
        std::fprintf(stderr, "  PERF GATE:\n%s", core::PerfGate::format(result).c_str());
    } else {
        std::printf("  perf gate: %s", core::PerfGate::format(result).c_str());
    }
    if (out != nullptr) {
        FILE* f = std::fopen(out, "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "15-terrain: could not write %s\n", out);
            status = 1;
        } else {
            const std::string json = report.to_json();
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
            std::printf("  wrote %s\n", out);
        }
    }
    device->wait_idle();
    return status;
}
