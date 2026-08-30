// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The application framework's implementation (M5.7, ADR-0023): the fixed-tick frame loop. The
// intellectual core — turning variable frame time into equal sim ticks — lives in the pure
// FixedTimestep (fixed_timestep.hpp); this file wires it to the World, the Schedule, the optional
// GPU, and the platform clock, and defines the ownership so the header can forward-declare the RHI.

#include "rime/app/application.hpp"

#include "rime/core/diagnostics/log.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/platform/clock.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/init.hpp"
#include "rime/platform/window.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/rhi/device.hpp"
#include "rime/rhi/swapchain.hpp"

namespace rime::app {
namespace {

// platform::Extent2D and rhi::Extent2D are distinct types on purpose — the window layer and the
// graphics layer share no headers — so every hand-off between them is explicit.
[[nodiscard]] rhi::Extent2D to_rhi(platform::Extent2D e) noexcept {
    return {e.width, e.height};
}

} // namespace

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
    if (config.windowed) {
        open_window();
    }
}

// Bring up a window + swapchain + the copy pass that puts a frame on it. EVERY failure here is a
// warning and a fall back to headless, never an error: a build that refuses to run without a
// display is a build CI cannot run, and the honest degradation is what lets one binary serve the
// workstation and the test farm. `windowed()` afterwards reports what actually happened.
void Application::open_window() {
    if (!device_) {
        RIME_WARN("app: windowed requires a GPU device — running headless");
        return;
    }
    if (!platform::init()) {
        RIME_WARN("app: platform::init() failed — running headless");
        return;
    }
    platform_started_ = true;

    platform::WindowDesc wd{};
    wd.title = config_.window_title.c_str();
    wd.width = config_.window_size.width;
    wd.height = config_.window_size.height;
    window_ = platform::create_window(wd);
    if (!window_) {
        RIME_WARN("app: no window backend or no display — running headless");
        return;
    }
    window_->show();

    rhi::SwapchainDesc sd{};
    sd.window = window_->native_handle();
    sd.extent = to_rhi(window_->framebuffer_size());
    swapchain_ = device_->create_swapchain(sd);
    if (!swapchain_) {
        RIME_WARN("app: this device cannot present to a window — running headless");
        window_.reset();
        return;
    }

    // The copy pass bakes the SWAPCHAIN's colour format, not ours — the driver chooses it, and a
    // graphics pipeline bakes its colour format in.
    present_pass_ = std::make_unique<render::PresentPass>(*device_, swapchain_->format());
    if (!present_pass_->valid()) {
        RIME_WARN("app: could not build the present pipeline — running headless");
        present_pass_.reset();
        swapchain_.reset();
        window_.reset();
        return;
    }
    presented_cmds_.resize(swapchain_->frames_in_flight() + 1u);
    RIME_INFO("app: presenting in a {}x{} window", sd.extent.width, sd.extent.height);
}

// Leave the GPU quiescent. The headless path ends every frame on submit_blocking and is already
// idle; the WINDOWED path is not — `Swapchain::present` queues work and returns, so when a loop
// exits the last frame may still be executing.
//
// This must run when the LOOP returns, not only in ~Application, and that distinction is the whole
// bug it fixes: an app owns its own GPU resources (a MeshRegistry, a SceneRenderer, textures) and
// destroys them between `run()` returning and the Application dying. Waiting only in the destructor
// still lets every one of those be freed out from under a frame in flight — which the validation
// layer reports as a wall of "currently in use by VkCommandBuffer", and which is silent corruption
// without it.
void Application::finish_gpu() {
    if (device_) {
        device_->wait_idle();
    }
}

// Drain the OS queue into the SAME snapshot a headless test injects into (ADR-0023 §5), so a sim
// system reading frame_input() cannot tell the two apart — which is what makes the headless input
// proof meaningful about the windowed build. Returns false when the user asked to close.
bool Application::pump_window() {
    if (!window_) {
        return true;
    }
    if (!platform::pump_events()) {
        return false;
    }
    platform::Event e{};
    while (platform::poll_event(e)) {
        post_input(e);
    }
    return !window_->should_close();
}

// Out-of-line so the header's forward-declared rhi::Device / render::RenderGraph are complete types
// here (their unique_ptr destructors need the full definition — the pimpl rule).
Application::~Application() {
    finish_gpu();
    // Order matters: the present pass and swapchain hold device resources, the window owns the
    // surface the swapchain was built on, and platform::shutdown() must come last.
    present_pass_.reset();
    swapchain_.reset();
    window_.reset();
    if (platform_started_) {
        platform::shutdown();
    }
}

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
    //
    // The profile zones below are the engine's FIRST placed zones — the hook has existed since M1.6
    // with a sink and no callers, which m12.0-perf turns into a measurement (ADR-0035 §2b asks for
    // "per-stage CPU ms", and this is where the stages are). They sit at STAGE granularity on
    // purpose: `report_zone` takes a lock and copies the sink, so a zone inside a per-entity loop
    // would cost more than it measures. With no sink installed a zone is two clock reads and an
    // early return.
    for (int i = 0; i < ticks; ++i) {
        RIME_PROFILE_ZONE("sim.tick");
        {
            // Before anything reads the world: poll the network, apply remote ops, route input
            // (ADR-0033 A5). Structural changes are legal — this is the main thread between phases.
            RIME_PROFILE_ZONE("sim.pre");
            run_stage(SimStage::PreSim);
        }
        {
            RIME_PROFILE_ZONE("sim.schedule");
            schedule_.run(world_, jobs_);
        }
        {
            RIME_PROFILE_ZONE("sim.transforms");
            ecs::propagate_transforms(world_, jobs_);
        }
        {
            // After the hierarchy is composed: where a physics PhysicsSync::step reads up-to-date
            // WorldTransforms, steps the sim, and writes poses back (stamping change detection) —
            // structural work that a parallel Schedule system could not do. This is exactly where
            // the old single on_fixed_tick hook ran, and it still does.
            RIME_PROFILE_ZONE("sim.post");
            run_stage(SimStage::PostSim);
        }
        {
            // Last: everything this tick will change has changed, so a consumer here sees final
            // state.
            RIME_PROFILE_ZONE("sim.publish");
            run_stage(SimStage::Publish);
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
        // owns compile + execute + submit.
        //
        // ACQUIRE FIRST, RESET SECOND — and the order is load-bearing, not stylistic.
        // `RenderGraph::reset()` destroys the previous frame's transient resources, including its
        // timestamp query pools. Headless that was always safe because every frame ended on
        // `submit_blocking`, so the previous frame was provably finished. A windowed frame ends on
        // `present`, which queues and returns, so frame N-1 may still be executing — and resetting
        // first destroys pools it is still using (the validation layer says exactly that).
        // `acquire_next_image` waits on this frame slot's in-flight fence before it returns
        // (swapchain_vulkan.cpp), so acquiring first is what makes the reset provably safe rather
        // than usually safe.
        rhi::TextureHandle backbuffer{};
        if (swapchain_ != nullptr) {
            // Check the window's real size every frame, not only on an out-of-date result: on
            // Wayland the surface reports a sentinel extent and NOTHING ever reports out-of-date
            // for a compositor resize (see Swapchain::ensure_extent).
            (void)swapchain_->ensure_extent(to_rhi(window_->framebuffer_size()));
            backbuffer = swapchain_->acquire_next_image();
            if (!backbuffer.is_valid()) {
                swapchain_->recreate(to_rhi(window_->framebuffer_size()));
                return; // no image this frame; the sim already ticked, which is the point of
                        // decoupling them. Note we return BEFORE reset(), so last frame's graph
                        // stays intact — nothing has been invalidated.
            }
            // The extent a windowed frame renders at is the SWAPCHAIN's, not the configured
            // offscreen one. Rendering at a fixed size and copying onto a resized window would crop
            // silently — the image simply stops halfway down and nothing reports it.
            ctx.extent = swapchain_->extent();
        }

        graph_->reset();

        {
            RIME_PROFILE_ZONE("frame.declare");
            render_(ctx);
        }

        // The callback said what it wants on screen; put it there. Declared AFTER the callback so
        // it is the last pass in the frame, and so a callback that sets nothing (or a headless run
        // of the very same callback) simply skips it.
        const bool presenting = backbuffer.is_valid() && ctx.present.is_valid();
        if (presenting) {
            const render::RGTexture target = graph_->import_texture(
                backbuffer, rhi::ResourceState::Undefined, swapchain_->extent());
            present_pass_->add(*graph_, ctx.present, target);
        }

        auto cmd = device_->begin_commands();
        {
            RIME_PROFILE_ZONE("frame.execute");
            graph_->execute(*cmd);
        }

        if (presenting) {
            // present() submits with the swapchain's own synchronization (wait acquired, signal
            // render-finished, in-flight fence) and queues the present, so the loop must NOT also
            // submit_blocking — that would submit the same command buffer twice.
            RIME_PROFILE_ZONE("frame.present");
            if (!swapchain_->present(*cmd)) {
                swapchain_->recreate(to_rhi(window_->framebuffer_size()));
            }
            // Keep it alive. `present` queued the work and returned, so this command buffer — and
            // the timestamp query pool it owns — is still in use by the GPU; letting it die at the
            // end of this scope is the use-after-free the validation layer reports as
            // "vkDestroyQueryPool ... currently in use". The assignment destroys the buffer this
            // slot held, which by the ring's sizing argument (see the member) is provably retired.
            presented_cmds_[presented_slot_] = std::move(cmd);
            presented_slot_ = (presented_slot_ + 1u) % presented_cmds_.size();
            // post_submit_ is deliberately NOT called here. Its whole contract is "submitted AND
            // completed", which is what makes RenderGraph::resolve_timings readable; a present is
            // pipelined and has not completed when it returns, so calling it would hand the perf
            // report timestamps from a frame still in flight. Silence beats plausible numbers.
        } else {
            {
                // Blocking, so this zone is the GPU's wall time as the CPU experiences it — the
                // honest number for a headless run, and the reason a `frame` timeline built from it
                // is not lying about where the time went.
                RIME_PROFILE_ZONE("frame.submit");
                device_->submit_blocking(*cmd);
            }
            // The one moment the frame's GPU timestamps are readable: submitted, completed, graph
            // not yet reset, command buffer not yet dead. See on_post_submit().
            if (post_submit_) {
                post_submit_(*graph_, *cmd);
            }
        }
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
    //
    // It pumps too, and that is not symmetry for its own sake: a WINDOWED app driven frame by frame
    // through step() would otherwise never service the OS queue, the compositor would stop
    // consuming presented images, and FIFO present would block forever on a fence — a hard hang,
    // not a slow frame. A close request cannot end a loop the caller owns, so it is recorded as a
    // quit request and the caller's own `quit_requested()` sees it.
    if (!pump_window()) {
        request_quit();
    }
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
        if (!pump_window()) {
            break; // the user closed the window — a bounded run still honours it
        }
        const std::uint64_t now = platform::Clock::now_ns();
        const double dt = static_cast<double>(now - last) * 1e-9;
        last = now;
        run_one_frame(dt);
    }
    finish_gpu(); // the loop's exit contract: no frame is still in flight when this returns
}

std::uint64_t Application::run() {
    // Unbounded real-clock loop: a sim system or the render callback ends it via request_quit()
    // (a windowed build also ends on window close — the ADR-0023 §4 seam). Headless with no quit
    // condition would spin forever, by design; tests drive step()/run_frames() instead.
    const std::uint64_t start = frame_index_;
    std::uint64_t last = platform::Clock::now_ns();
    while (!quit_) {
        if (!pump_window()) {
            break; // window close ends the loop — the other half of ADR-0023 §4's seam
        }
        const std::uint64_t now = platform::Clock::now_ns();
        const double dt = static_cast<double>(now - last) * 1e-9;
        last = now;
        run_one_frame(dt);
    }
    finish_gpu(); // the loop's exit contract: no frame is still in flight when this returns
    return frame_index_ - start;
}

} // namespace rime::app
