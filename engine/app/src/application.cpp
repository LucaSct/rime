// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The application framework's implementation (M5.7, ADR-0023): the fixed-tick frame loop. The
// intellectual core — turning variable frame time into equal sim ticks — lives in the pure
// FixedTimestep (fixed_timestep.hpp); this file wires it to the World, the Schedule, the optional
// GPU, and the platform clock, and defines the ownership so the header can forward-declare the RHI.

#include "rime/app/application.hpp"

#include "rime/core/diagnostics/log.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/platform/clock.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/rhi/device.hpp"

namespace rime::app {

Application::Application(const AppConfig& config)
    : config_(config), jobs_(config.worker_threads),
      timestep_(FixedTimestep::from_hz(config.tick_hz, config.max_ticks_per_frame)) {
    // The GPU is owned only when asked for. A missing backend (RIME_RHI_VULKAN off, or no lavapipe)
    // is not fatal — the app simply runs GPU-free and the render callback sees a null device — so a
    // pure-sim tool on a headless build without Vulkan still works. Rendering code must tolerate
    // it.
    if (config.gpu) {
        device_ = rhi::create_device({});
        if (device_) {
            graph_ = std::make_unique<render::RenderGraph>(*device_);
        } else {
            RIME_WARN(
                "app: config.gpu set but no Vulkan device could be created — running GPU-free");
        }
    }
}

// Out-of-line so the header's forward-declared rhi::Device / render::RenderGraph are complete types
// here (their unique_ptr destructors need the full definition — the pimpl rule).
Application::~Application() = default;

void Application::run_ticks(int ticks) {
    // One tick = one deterministic step of the world: run the scheduled systems, then compose the
    // transform hierarchy so a tick leaves WorldTransforms consistent for the render that follows.
    // Every tick advances the sim by exactly fixed_dt() — the invariant the determinism proof and
    // M11 netcode both rest on (ADR-0023 §1). Systems read that constant dt by capturing it.
    for (int i = 0; i < ticks; ++i) {
        schedule_.run(world_, jobs_);
        ecs::propagate_transforms(world_, jobs_);
        // The per-tick hook runs last, after the hierarchy is composed: this is where a physics
        // PhysicsSync::step reads up-to-date WorldTransforms, steps the sim, and writes poses back
        // (stamping change detection) — structural work that a parallel Schedule system could not
        // do. See docs/design/simulation-tick.md for the canonical order.
        if (fixed_tick_) {
            fixed_tick_(world_, timestep_.fixed_dt);
        }
        ++tick_count_;
    }
}

void Application::render_frame(double alpha) {
    if (!render_) {
        return; // a pure-sim app declares no render frame
    }
    FrameContext ctx{world_,
                     jobs_,
                     alpha,
                     frame_index_,
                     frame_input_,
                     graph_.get(),
                     device_.get(),
                     config_.render_extent};

    if (graph_ && device_) {
        // GPU frame: the callback declares passes into a fresh graph and exports a target; the loop
        // owns compile + execute + submit. (Present, for a windowed build, hangs off the exported
        // target — the ADR-0023 §4 seam; headless/streamed builds read it back or tap it instead.)
        graph_->reset();
        render_(ctx);
        auto cmd = device_->begin_commands();
        graph_->execute(*cmd);
        device_->submit_blocking(*cmd);
    } else {
        // GPU-free: the callback still runs (it might do a CPU capture, drive a headless probe, or
        // just request_quit()), it just gets a null graph/device.
        render_(ctx);
    }
}

void Application::run_one_frame(double frame_dt) {
    // Snapshot input at the frame edge: this frame's ticks and render all see the events posted
    // since the last frame, and nothing posted mid-frame (ADR-0023 §5). swap-then-clear moves the
    // pending events in and leaves the pending queue empty for the next frame with no allocation.
    frame_input_.swap(pending_input_);
    pending_input_.clear();

    const FixedTimestep::Step step = timestep_.advance(frame_dt);
    run_ticks(step.ticks);
    render_frame(step.alpha);
    ++frame_index_;
}

FixedTimestep::Step Application::step(double frame_dt) {
    // Same body as run_one_frame but hands back the tick/alpha breakdown for tests and tools; kept
    // separate so the hot loop paths (run/run_frames) don't pay to return it.
    frame_input_.swap(pending_input_);
    pending_input_.clear();

    const FixedTimestep::Step s = timestep_.advance(frame_dt);
    run_ticks(s.ticks);
    render_frame(s.alpha);
    ++frame_index_;
    return s;
}

void Application::run_frames(int frames) {
    // Bounded headless run off the real monotonic clock — what CI executes (there is no window to
    // close the loop). The first frame's dt is ~0 (baseline), so it runs zero ticks and just
    // establishes the clock; subsequent frames carry real elapsed time.
    std::uint64_t last = platform::Clock::now_ns();
    for (int i = 0; i < frames && !quit_; ++i) {
        const std::uint64_t now = platform::Clock::now_ns();
        const double dt = static_cast<double>(now - last) * 1e-9;
        last = now;
        run_one_frame(dt);
    }
}

std::uint64_t Application::run() {
    // Unbounded real-clock loop: a sim system or the render callback ends it via request_quit()
    // (a windowed build also ends on window close — the ADR-0023 §4 seam). Headless with no quit
    // condition would spin forever, by design; tests drive step()/run_frames() instead.
    const std::uint64_t start = frame_index_;
    std::uint64_t last = platform::Clock::now_ns();
    while (!quit_) {
        const std::uint64_t now = platform::Clock::now_ns();
        const double dt = static_cast<double>(now - last) * 1e-9;
        last = now;
        run_one_frame(dt);
    }
    return frame_index_ - start;
}

} // namespace rime::app
