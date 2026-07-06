// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "rime/app/fixed_timestep.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/ecs/schedule.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/event.hpp"
#include "rime/rhi/types.hpp" // Extent2D only — no Vulkan, no backend

// The application framework (M5.7, ADR-0023): `rime::app::Application` ties the engine's modules
// into a runnable whole and owns the frame loop. Its defining feature is a FIXED SIMULATION TICK
// decoupled from the render frame — the sim advances in equal `fixed_dt` steps via a time
// accumulator (fixed_timestep.hpp), the render frame runs once per loop iteration at whatever rate
// it manages, and an interpolation `alpha` bridges them. That decoupling is a multiplayer (M11)
// seam kept from day one; the determinism it buys is proven in tests/app.
//
// Headless-first, deliberately: this is where the engine is developed and CI'd on a machine with no
// display (lavapipe), so the whole loop — ticks, input, and optional GPU rendering into an
// offscreen target — runs and is verified without a window. The windowed/swapchain PRESENT path is
// a documented seam (ADR-0023 §4), wired when a display-bearing sample (07-first-light) needs it.
//
// `Application` owns the JobSystem, the ECS World, the sim Schedule, and the FixedTimestep; it
// OPTIONALLY owns an rhi::Device + render::RenderGraph (config.gpu), so pure-sim tools and tests
// pay for no GPU. Rendering is a per-frame callback handed a FrameContext. `rime_hello` stays the
// trivial M0 launcher; this is the real entry point a game or sample drives.
namespace rime::rhi {
class Device;
}

namespace rime::render {
class RenderGraph;
}

namespace rime::app {

// How to build an Application. Defaults describe a headless, GPU-free, 60 Hz simulation — the
// shape a sim test or tool wants; a rendering app flips `gpu` and sets a `render_extent`.
struct AppConfig {
    double tick_hz = 60.0;         // simulation ticks per second (the fixed step)
    int max_ticks_per_frame = 8;   // spiral-of-death clamp (FixedTimestep)
    unsigned worker_threads = 0;   // JobSystem workers; 0 = hardware_concurrency()-1
    bool gpu = false;              // create an rhi::Device + render::RenderGraph for the frame
    rhi::Extent2D render_extent{}; // offscreen render size when gpu (0×0 = none declared yet)
};

// Everything a render callback is handed for one frame. `world` is post-tick (already simulated
// this frame); `alpha` is where the render sits between the last two ticks (v0 draws the latest
// tick and ignores it — the interpolation seam, ADR-0023 §3); `input` is the frame's event
// snapshot. `graph`/`device` are non-null only when the app owns a GPU: declare passes into
// `graph` and export a target — the loop executes it after the callback returns.
struct FrameContext {
    ecs::World& world;
    core::JobSystem& jobs;
    double alpha = 0.0;
    std::uint64_t frame_index = 0;
    std::span<const platform::Event> input = {};
    render::RenderGraph* graph = nullptr;
    rhi::Device* device = nullptr;
    rhi::Extent2D extent{};
};

class Application {
public:
    explicit Application(const AppConfig& config = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // ── The pieces the app owns (a game wires its systems/entities through these) ───────────
    [[nodiscard]] ecs::World& world() noexcept { return world_; }

    [[nodiscard]] ecs::Schedule& schedule() noexcept { return schedule_; }

    [[nodiscard]] core::JobSystem& jobs() noexcept { return jobs_; }

    [[nodiscard]] FixedTimestep& timestep() noexcept { return timestep_; }

    // The device the app owns, or nullptr when GPU-free (config.gpu == false) or when device
    // creation failed (no Vulkan backend / no lavapipe). Rendering code must tolerate null.
    [[nodiscard]] rhi::Device* device() noexcept { return device_.get(); }

    // The render graph the loop executes each frame, or nullptr when GPU-free. The frame's passes
    // are declared into it (via the render callback's FrameContext) and executed by the loop; this
    // accessor is how a capture/present/stream step reads an exported target's physical handle
    // AFTER the frame ran (a headless self-check, an engine/stream tap). Handles from the last
    // frame stay valid until the next frame's reset().
    [[nodiscard]] render::RenderGraph* graph() noexcept { return graph_.get(); }

    // The constant a simulation tick advances the world by. Systems integrate against THIS (not a
    // frame dt) — capture it by value in a system body; it never changes for the app's lifetime.
    [[nodiscard]] double fixed_dt() const noexcept { return timestep_.fixed_dt; }

    // Set the once-per-frame render callback (replacing any previous). Optional: a pure-sim app
    // sets none.
    using RenderFn = std::function<void(FrameContext&)>;

    void on_render(RenderFn fn) { render_ = std::move(fn); }

    // ── Input ───────────────────────────────────────────────────────────────────────────────
    // Queue an event for the NEXT frame's snapshot. Windowed, the loop fills this from the OS
    // pump; headless, a test (or a scripted harness) injects events the same way — which is how
    // "input reaches a sim system" is proven without a window (ADR-0023 §5).
    void post_input(const platform::Event& e) { pending_input_.push_back(e); }

    // The current frame's input snapshot — what this frame's ticks (and its render) see. A sim
    // system that needs input captures the Application and reads this during its body (the v0
    // routing; an ECS-native input resource is the later refinement, ADR-0023 §5). Refilled at each
    // frame's edge; valid to read while a tick runs.
    [[nodiscard]] std::span<const platform::Event> frame_input() const noexcept {
        return frame_input_;
    }

    // ── Driving the loop ─────────────────────────────────────────────────────────────────────
    // One frame with an EXPLICIT dt and no clock: advance the accumulator, run the due ticks
    // (Schedule + propagate_transforms), then render once. Deterministic — the determinism proof
    // drives the app this way with different `dt` patterns. Returns the tick/alpha breakdown.
    FixedTimestep::Step step(double frame_dt);

    // Headless: run exactly `frames` iterations off the real monotonic clock. The bounded loop CI
    // runs (no window to close it).
    void run_frames(int frames);

    // Run off the real clock until quit is requested (a sim system or the render callback calls
    // request_quit(); a windowed build also ends on window close — that path is the ADR-0023 §4
    // seam). Returns the number of frames run.
    std::uint64_t run();

    void request_quit() noexcept { quit_ = true; }

    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

    // ── Introspection (the proofs read these) ────────────────────────────────────────────────
    [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }

    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_count_; }

private:
    void run_ticks(int ticks);       // run `ticks` simulation steps
    void render_frame(double alpha); // build + execute the render frame (if a callback/GPU exist)
    void run_one_frame(double frame_dt);

    AppConfig config_;
    core::JobSystem jobs_;
    ecs::World world_;
    ecs::Schedule schedule_;
    FixedTimestep timestep_;
    RenderFn render_;

    std::unique_ptr<rhi::Device> device_;        // owned when config.gpu
    std::unique_ptr<render::RenderGraph> graph_; // owned when a device exists

    std::vector<platform::Event> pending_input_; // queued for the next frame
    std::vector<platform::Event> frame_input_;   // this frame's snapshot (what systems see)

    std::uint64_t frame_index_ = 0;
    std::uint64_t tick_count_ = 0;
    bool quit_ = false;
};

} // namespace rime::app
