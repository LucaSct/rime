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

void Application::on_fixed_tick(TickFn fn) {
    // Sugar over the ordered stage: the historical single hook is simply the one PostSim entry this
    // function owns. Replacing it in place (rather than appending) is what keeps the documented
    // "replacing slot" behaviour every existing caller was written against.
    auto& post_sim = stages_[static_cast<std::size_t>(SimStage::PostSim)];
    if (legacy_tick_index_ >= 0) {
        post_sim[static_cast<std::size_t>(legacy_tick_index_)] = std::move(fn);
        return;
    }
    legacy_tick_index_ = static_cast<std::ptrdiff_t>(post_sim.size());
    post_sim.push_back(std::move(fn));
}

void Application::add_sim_stage(SimStage stage, TickFn fn) {
    stages_[static_cast<std::size_t>(stage)].push_back(std::move(fn));
}

void Application::run_stage(SimStage stage) {
    // Indexed by value rather than iterated by reference on purpose: a step is allowed to register
    // another step (a spawner wiring up a subsystem mid-tick), which can reallocate the vector out
    // from under a held iterator. Steps added during a tick run on the NEXT one, never this one.
    auto& steps = stages_[static_cast<std::size_t>(stage)];
    for (std::size_t i = 0, n = steps.size(); i < n; ++i) {
        if (steps[i]) {
            steps[i](world_, timestep_.fixed_dt);
        }
    }
}

void Application::run_ticks(int ticks) {
    // One tick = one deterministic step of the world, in the canonical order
    // (docs/design/simulation-tick.md): pre-sim hooks, the scheduled systems, then the transform
    // hierarchy composed so a tick leaves WorldTransforms consistent for the render that follows,
    // then the post-sim and publish hooks. Every tick advances the sim by exactly fixed_dt() — the
    // invariant the determinism proof and M11 netcode both rest on (ADR-0023 §1). Systems read that
    // constant dt by capturing it.
    for (int i = 0; i < ticks; ++i) {
        // Before anything reads the world: poll the network, apply remote ops, route input
        // (ADR-0033 A5). Structural changes are legal — this is the main thread between phases.
        run_stage(SimStage::PreSim);
        schedule_.run(world_, jobs_);
        ecs::propagate_transforms(world_, jobs_);
        // After the hierarchy is composed: where a physics PhysicsSync::step reads up-to-date
        // WorldTransforms, steps the sim, and writes poses back (stamping change detection) —
        // structural work that a parallel Schedule system could not do. This is exactly where the
        // old single on_fixed_tick hook ran, and it still does.
        run_stage(SimStage::PostSim);
        // Last: everything this tick will change has changed, so a consumer here sees final state.
        run_stage(SimStage::Publish);
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
