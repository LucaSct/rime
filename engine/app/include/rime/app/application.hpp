// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rime/app/fixed_timestep.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/ecs/schedule.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/event.hpp"
#include "rime/render/render_graph.hpp" // RGTexture (FrameContext::present) — no backend headers
#include "rime/rhi/types.hpp"           // Extent2D only — no Vulkan, no backend

// The application framework (M5.7, ADR-0023): `rime::app::Application` ties the engine's modules
// into a runnable whole and owns the frame loop. Its defining feature is a FIXED SIMULATION TICK
// decoupled from the render frame — the sim advances in equal `fixed_dt` steps via a time
// accumulator (fixed_timestep.hpp), the render frame runs once per loop iteration at whatever rate
// it manages, and an interpolation `alpha` bridges them. That decoupling is a multiplayer (M11)
// seam kept from day one; the determinism it buys is proven in tests/app.
//
// Headless-first, deliberately: this is where the engine is developed and CI'd on a machine with no
// display (lavapipe), so the whole loop — ticks, input, and optional GPU rendering into an
// offscreen target — runs and is verified without a window.
//
// WINDOWED PRESENT (m13.3a) finally closes ADR-0023 §4's seam, which had been open since M5.7 and
// which `07-first-light --windowed` has been printing an honest "needs a display; running headless
// instead" against ever since. Set `AppConfig::windowed` and the loop additionally owns a
// platform::Window and an rhi::Swapchain: it pumps the OS event queue into the same input snapshot
// a headless test injects into, acquires a backbuffer, copies whatever the render callback set as
// `FrameContext::present` onto it, and presents.
//
// **Windowed is a REQUEST, never a requirement.** No display, no window backend, or no surface
// support degrades to exactly the headless path with one warning — because the alternative is a
// binary that runs in CI only by accident of which machine it landed on. `windowed()` reports what
// actually happened, and it is what a proof should assert against rather than the config field.
//
// `Application` owns the JobSystem, the ECS World, the sim Schedule, and the FixedTimestep; it
// OPTIONALLY owns an rhi::Device + render::RenderGraph (config.gpu), so pure-sim tools and tests
// pay for no GPU. Rendering is a per-frame callback handed a FrameContext. `rime_hello` stays the
// trivial M0 launcher; this is the real entry point a game or sample drives.
namespace rime::rhi {
class Device;
class CommandBuffer;
class Swapchain;
} // namespace rime::rhi

namespace rime::platform {
class Window;
}

namespace rime::render {
class PresentPass;
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

    // ── Windowed present (m13.3a, ADR-0023 §4) ───────────────────────────────────────────────
    // Requires `gpu`: presenting means a swapchain, and a swapchain means a device. Requesting a
    // window without a display is not an error — see the class comment.
    bool windowed = false;
    std::string window_title = "Rime";
    rhi::Extent2D window_size{1280, 720};
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
    // Seconds of REAL time this frame covers — the same dt the accumulator was just advanced by,
    // and not a fixed step. It is here for the things that are presentation rather than simulation
    // and therefore must not run on the tick: a free camera, a UI animation, a smoothed readout
    // (m13.3c). Anything the server arbitrates uses `Application::fixed_dt()` instead; the whole
    // point of the split is that this number varies and that one does not.
    double frame_dt = 0.0;
    std::uint64_t frame_index = 0;
    std::span<const platform::Event> input = {};
    render::RenderGraph* graph = nullptr;
    rhi::Device* device = nullptr;
    rhi::Extent2D extent{};

    // OUT — the one field a callback WRITES. Set it to the image you want on screen and the loop
    // copies that onto the acquired backbuffer and presents it (m13.3a). Ignored when the app is
    // not windowed, so the same callback serves a headless run unchanged, which is what lets a
    // display-bearing sample keep exactly one render path instead of two.
    render::RGTexture present{};
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

    // Did a window and swapchain actually come up? False on a headless build, a machine with no
    // display, or a device without surface support — even when `config.windowed` was set. Assert
    // against THIS, never against the config, or a proof passes on a workstation and means nothing
    // in CI.
    [[nodiscard]] bool windowed() const noexcept { return swapchain_ != nullptr; }

    // The window, or nullptr when not windowed. Exposed so an app can read its framebuffer size or
    // title it; the loop owns its lifetime and its event pump.
    [[nodiscard]] platform::Window* window() noexcept { return window_.get(); }

    // The constant a simulation tick advances the world by. Systems integrate against THIS (not a
    // frame dt) — capture it by value in a system body; it never changes for the app's lifetime.
    [[nodiscard]] double fixed_dt() const noexcept { return timestep_.fixed_dt; }

    // Set the once-per-frame render callback (replacing any previous). Optional: a pure-sim app
    // sets none.
    using RenderFn = std::function<void(FrameContext&)>;

    void on_render(RenderFn fn) { render_ = std::move(fn); }

    // Set the after-the-GPU-is-done callback (replacing any previous). It runs once per GPU frame,
    // immediately after the frame's command buffer has been submitted AND completed, with the
    // graph still holding this frame's passes and the command buffer still alive.
    //
    // That is a narrow window, and it exists for exactly one reason: it is the ONLY moment
    // `RenderGraph::resolve_timings(cmd)` can read the frame's GPU timestamps — the graph resets at
    // the top of the next frame, and the command buffer dies at the bottom of this one. Without
    // this seam, per-pass GPU cost is unreachable from anything built on `Application`, which is
    // most of the engine (m12.0-perf / ADR-0035 §2b needs it for the hardware report). Never called
    // for a GPU-free app: there is no submission to be after.
    using PostSubmitFn = std::function<void(render::RenderGraph&, rhi::CommandBuffer&)>;

    void on_post_submit(PostSubmitFn fn) { post_submit_ = std::move(fn); }

    // Set the per-FIXED-TICK callback (replacing any previous). It runs once per simulation tick,
    // after the Schedule and transform propagation — the place per-tick work that must NOT run
    // inside a parallel query belongs: a physics PhysicsSync::step (its reconcile adds/removes
    // components — a structural change), a spawner draining a command queue, an authoritative
    // integrator. It is handed the World and the fixed dt (the same constant every tick). Unlike a
    // Schedule system it may make structural changes, because it runs on the main thread between
    // phases, not concurrently with anything. Optional: a pure-Schedule app sets none.
    using TickFn = std::function<void(ecs::World&, double)>;

    void on_fixed_tick(TickFn fn);

    // ── The ordered sim stage (ADR-0032 §8, pulled forward by ADR-0033 A5) ──────────────────
    // The single replacing hook above, generalized into named hook points in the canonical
    // per-tick order (docs/design/simulation-tick.md). The phases are the GAPS around the two
    // steps the loop already owns, because those steps — the Schedule and transform propagation —
    // are neither optional nor hooks:
    //
    //     PreSim → [Schedule] → [propagate_transforms] → PostSim → Publish
    //
    //   PreSim  — before any system runs: poll the network, apply remote ops, route input. Like
    //             every stage it runs on the main thread between phases, so structural world
    //             changes are legal here (the same rule the single hook always had).
    //   PostSim — after the hierarchy is composed and WorldTransforms are current. This is where
    //             the physics bridge (physics::PhysicsSync::step — reconcile, step, write-back)
    //             belongs, and it is the EXACT position on_fixed_tick has always occupied, so
    //             existing callers neither move nor change. Named for the tick, not for physics:
    //             `app` deliberately does not depend on `physics` (simulation-tick.md §1).
    //   Publish — after everything the tick will mutate has mutated: drain change sets
    //             (for_each_changed), publish snapshots, run a change-tracker. A consumer here
    //             sees the tick's FINAL state, which is the whole reason it is a separate phase.
    //
    // Networking is the first customer that needs two of these at once: poll and apply remote ops
    // in PreSim, publish snapshots in Publish, with the sim in between (ADR-0033 A5).
    enum class SimStage : std::uint8_t { PreSim = 0, PostSim = 1, Publish = 2 };

    static constexpr std::size_t kSimStageCount = 3;

    // Add a per-tick step at `stage`. Steps within a stage run in REGISTRATION ORDER — stable and
    // documented, so the order an app wires its modules is the order they observe the tick. Steps
    // are never removed (v1: teardown is process exit; an unregister seam is additive if a real
    // customer appears). Zero-cost when unused: an empty vector per stage is one branch per tick.
    void add_sim_stage(SimStage stage, TickFn fn);

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
    // When this returns the GPU is IDLE — no frame is still in flight. That matters only for a
    // windowed run (present queues work and returns, where the headless path blocks on submit), and
    // it is what makes it safe for a caller to destroy its own meshes, textures and renderer
    // immediately afterwards.
    void run_frames(int frames);

    // Run off the real clock until quit is requested (a sim system or the render callback calls
    // request_quit(); a windowed build also ends on window close — that path is the ADR-0023 §4
    // seam). Returns the number of frames run.
    std::uint64_t run();

    void request_quit() noexcept { quit_ = true; }

    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

    // Block until no frame is still executing on the GPU.
    //
    // `run()` and `run_frames()` do this for you at loop exit, which is why their contract can
    // promise the GPU is idle when they return. `step()` CANNOT: it is one frame, it has no exit,
    // and idling inside it would serialise the pipeline it exists to keep full. So a caller who
    // drives the loop frame-by-frame owns this call, and must make it before destroying anything
    // the frames rendered with — its meshes, textures, materials, renderer.
    //
    // Headless this is redundant (a headless frame ends on `submit_blocking`, which has already
    // finished it) and windowed it is required (`present` queues and returns). That asymmetry is
    // the m13.3a root cause in its last uncovered place, and it is worse than it looks: the mode
    // that needs the call is the one CI never runs, so the validation layer reports it on a
    // developer's desktop and nowhere else. Cheap and harmless when unnecessary — just call it.
    void finish_gpu();

    // ── Introspection (the proofs read these) ────────────────────────────────────────────────
    [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }

    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_count_; }

private:
    void run_stage(SimStage stage); // run every step registered at one phase, in order
    void run_ticks(int ticks);      // run `ticks` simulation steps
    // Build + execute the render frame (if a callback/GPU exist). Takes the frame's dt as well as
    // its alpha because both go into the FrameContext the callback sees.
    void render_frame(double alpha, double frame_dt);
    void run_one_frame(double frame_dt);
    void open_window(); // windowed setup; leaves the app headless on any failure
    bool pump_window(); // drain the OS queue into the input snapshot; false = close requested

    AppConfig config_;
    core::JobSystem jobs_;
    ecs::World world_;
    ecs::Schedule schedule_;
    FixedTimestep timestep_;
    RenderFn render_;
    PostSubmitFn post_submit_;
    std::array<std::vector<TickFn>, kSimStageCount> stages_;
    // Where on_fixed_tick's entry lives inside stages_[PostSim], or -1 if it was never set. Keeping
    // the index is what preserves the old REPLACING semantics on top of a list: a second
    // on_fixed_tick overwrites that one entry in place rather than appending a duplicate, and it
    // holds its original position relative to anything added around it. One mechanism, not two.
    std::ptrdiff_t legacy_tick_index_ = -1;

    std::unique_ptr<rhi::Device> device_;        // owned when config.gpu
    std::unique_ptr<render::RenderGraph> graph_; // owned when a device exists

    // Windowed present (m13.3a). All three are null together: either the window, the swapchain and
    // the copy pass all came up, or the app is headless. There is no half-windowed state.
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<rhi::Swapchain> swapchain_;
    std::unique_ptr<render::PresentPass> present_pass_;
    bool platform_started_ = false; // we called platform::init() and owe it a shutdown()

    // Presented frames' command buffers, kept alive until their GPU work is provably done.
    // `Swapchain::present` does not wait, and a command buffer owns its timestamp query pool — so
    // letting one die at the end of the frame that submitted it destroys a pool the GPU is still
    // reading. Sized `frames_in_flight() + 1`: with only that many fence slots, a buffer this old
    // was submitted to a slot that has since been re-acquired, and acquire waits on its fence.
    // Empty (and unused) headless, where submit_blocking has already finished the frame.
    std::vector<std::unique_ptr<rhi::CommandBuffer>> presented_cmds_;
    std::size_t presented_slot_ = 0;

    std::vector<platform::Event> pending_input_; // queued for the next frame
    std::vector<platform::Event> frame_input_;   // this frame's snapshot (what systems see)

    std::uint64_t frame_index_ = 0;
    std::uint64_t tick_count_ = 0;
    bool quit_ = false;
};

} // namespace rime::app
