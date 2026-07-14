// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the application framework's frame loop (M5.7, ADR-0023):
//
//  (1) HEADLESS SMOKE — a sim system runs once per tick and mutates the World; frame/tick counters
//      track (GPU-free).
//  (2) TICK DETERMINISM — the headline. The same simulation driven with two DIFFERENT frame-pacing
//      patterns, normalized to the same tick count, yields a BIT-IDENTICAL world. This is the
//      property M11 netcode rests on: world state is a pure function of ticks elapsed, not of how
//      wall-clock time was sliced into frames (GPU-free).
//  (3) INPUT REACHES A SIM SYSTEM — an injected event appears in the frame snapshot a system reads,
//      without a window (GPU-free; the headless input seam, ADR-0023 §5).
//  (4) THE GPU-OWNING LOOP — with config.gpu, the Application owns a device + graph and drives a
//      render callback that declares and executes a real pass each frame (self-guards on a device,
//      like every GPU proof since M3.3).

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/system.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/render/render_graph.hpp"

namespace {

using namespace rime;
using rime::app::AppConfig;
using rime::app::Application;
using rime::app::FrameContext;

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// ── Test components ─────────────────────────────────────────────────────────────────────────────
struct Counter {
    int value = 0;
};

struct Position {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Velocity {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct InputFlag {
    int key_downs = 0;
};

TEST_CASE("app: the fixed tick drives a sim system once per tick (M5.7)") {
    Application app(AppConfig{}); // headless, GPU-free, 60 Hz
    ecs::World& w = app.world();
    const ecs::Entity e = w.spawn_with(Counter{0});

    // A system that bumps every Counter — one bump per tick. Writes Counter, reads nothing.
    app.schedule().add("count",
                       ecs::SystemAccess{{}, ecs::signature_of<Counter>(w)},
                       [](ecs::World& world, core::JobSystem&, ecs::CommandBuffer&) {
                           world.query<Counter>().for_each([](Counter& c) { c.value += 1; });
                       });

    // Ten whole-tick frames → ten ticks → ten bumps. frame_index and tick_count agree with a
    // one-tick-per-frame cadence.
    const double fd = app.fixed_dt();
    for (int i = 0; i < 10; ++i) {
        const auto step = app.step(fd);
        CHECK(step.ticks == 1);
    }
    CHECK(app.tick_count() == 10);
    CHECK(app.frame_index() == 10);
    CHECK(w.get<Counter>(e)->value == 10);

    // The real-clock headless driver (what CI runs — no window to end the loop) advances exactly
    // the requested number of frames. Its tick count is wall-clock-dependent, so assert only that
    // it ran forward and stayed consistent (counter == ticks, since every tick bumps it once).
    app.run_frames(5);
    CHECK(app.frame_index() == 15);
    CHECK(app.tick_count() >= 10);
    CHECK(w.get<Counter>(e)->value == static_cast<int>(app.tick_count()));
}

TEST_CASE("app: the per-fixed-tick hook runs once per tick and may change structure (M7.8)") {
    Application app(AppConfig{}); // headless, GPU-free
    ecs::World& w = app.world();

    // The hook counts ticks, checks it is handed the fixed dt, and — proving it may make STRUCTURAL
    // changes a parallel Schedule system cannot — spawns one entity per tick (a
    // PhysicsSync::reconcile adds/removes components exactly like this).
    int hook_ticks = 0;
    double seen_dt = 0.0;
    app.on_fixed_tick([&](ecs::World& world, double dt) {
        ++hook_ticks;
        seen_dt = dt;
        (void)world.spawn_with(Counter{hook_ticks});
    });

    for (int i = 0; i < 8; ++i) {
        const auto step = app.step(app.fixed_dt());
        CHECK(step.ticks == 1);
    }
    CHECK(hook_ticks == 8);
    CHECK(hook_ticks == static_cast<int>(app.tick_count()));
    CHECK(seen_dt == doctest::Approx(app.fixed_dt()));
    CHECK(w.query<Counter>().count() == 8); // the per-tick structural spawns all took effect
}

TEST_CASE("app: tick determinism — pacing does not change the world (M5.7)") {
    // Build the same scene + integrate system into an app: N particles carried by their velocities,
    // one Euler step of the constant fixed_dt per tick.
    const auto build = [](Application& app) {
        ecs::World& w = app.world();
        for (int i = 0; i < 40; ++i) {
            const double f = static_cast<double>(i);
            (void)w.spawn_with(Position{f, -f, 0.5 * f}, Velocity{1.0 + f, -0.5, 0.25 * f - 3.0});
        }
        const double dt = app.fixed_dt();
        app.schedule().add(
            "integrate",
            ecs::SystemAccess{ecs::signature_of<Velocity>(w), ecs::signature_of<Position>(w)},
            [dt](ecs::World& world, core::JobSystem&, ecs::CommandBuffer&) {
                world.query<Position, Velocity>().for_each([dt](Position& p, Velocity& v) {
                    p.x += v.x * dt;
                    p.y += v.y * dt;
                    p.z += v.z * dt;
                });
            });
    };

    Application a(AppConfig{});
    Application b(AppConfig{});
    build(a);
    build(b);
    const double fd = a.fixed_dt();

    // A: a steady one-tick-per-frame cadence. B: a deliberately ragged one — big frames, tiny
    // frames, sub-tick frames — the pattern a real variable frame rate produces.
    for (int i = 0; i < 100; ++i)
        a.step(fd);
    const double ragged[5] = {2.0 * fd, 0.3 * fd, 0.7 * fd, 0.5 * fd, 1.5 * fd};
    for (int i = 0; i < 100; ++i)
        b.step(ragged[i % 5]);

    // A tick is a pure function of the tick COUNT, so normalize the two to the same count
    // (advancing whichever trails by whole-tick frames) before comparing — that is the invariant,
    // stated operationally.
    while (a.tick_count() < b.tick_count())
        a.step(fd);
    while (b.tick_count() < a.tick_count())
        b.step(fd);
    REQUIRE(a.tick_count() == b.tick_count());
    REQUIRE(a.tick_count() > 0);

    // Same worlds built the same way ⇒ same archetype/iteration order, so column i corresponds. The
    // claim is BIT-identical, not approximately equal: identical ops, identical count, identical
    // order.
    std::vector<Position> pa, pb;
    a.world().query<Position>().for_each([&](Position& p) { pa.push_back(p); });
    b.world().query<Position>().for_each([&](Position& p) { pb.push_back(p); });
    REQUIRE(pa.size() == pb.size());
    REQUIRE(pa.size() == 40u);
    bool all_bit_identical = true;
    for (std::size_t i = 0; i < pa.size(); ++i) {
        if (pa[i].x != pb[i].x || pa[i].y != pb[i].y || pa[i].z != pb[i].z)
            all_bit_identical = false;
    }
    CHECK(all_bit_identical);
    // And it actually moved — a world frozen at its start would pass vacuously.
    CHECK(pa[1].x != 1.0);
}

TEST_CASE("app: an injected input event reaches a sim system (M5.7)") {
    Application app(AppConfig{});
    ecs::World& w = app.world();
    const ecs::Entity e = w.spawn_with(InputFlag{0});

    // The system reads THIS frame's input snapshot (the v0 routing: capture the app, read
    // frame_input) and counts key-down events into the flag.
    app.schedule().add("read-input",
                       ecs::SystemAccess{{}, ecs::signature_of<InputFlag>(w)},
                       [&app](ecs::World& world, core::JobSystem&, ecs::CommandBuffer&) {
                           int downs = 0;
                           for (const platform::Event& ev : app.frame_input())
                               if (ev.type == platform::EventType::KeyDown)
                                   ++downs;
                           if (downs > 0)
                               world.query<InputFlag>().for_each(
                                   [downs](InputFlag& f) { f.key_downs += downs; });
                       });

    const double fd = app.fixed_dt();

    // Frame 1: no input posted → nothing to see.
    app.step(fd);
    CHECK(w.get<InputFlag>(e)->key_downs == 0);

    // Frame 2: inject a Space key-down before the frame → the frame's tick sees it.
    platform::Event ev{};
    ev.type = platform::EventType::KeyDown;
    ev.key.key = platform::Key::Space;
    app.post_input(ev);
    app.step(fd);
    CHECK(w.get<InputFlag>(e)->key_downs == 1);

    // Frame 3: no new input → the snapshot is empty again, the flag holds (input is per-frame, not
    // sticky).
    app.step(fd);
    CHECK(w.get<InputFlag>(e)->key_downs == 1);
}

TEST_CASE("app: the GPU-owning loop drives a render callback each frame (M5.7)") {
    AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {64, 48};
    Application app(cfg);

    if (!app.device()) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but the app could not create a Vulkan device");
        MESSAGE("no Vulkan device available — skipping the GPU render-loop proof");
        return;
    }

    // The callback gets a live graph + device and the configured extent. It declares a real frame
    // (a cleared HDR target, exported) so the loop's compile→execute→submit runs end to end.
    int renders = 0;
    app.on_render([&](FrameContext& ctx) {
        ++renders;
        REQUIRE(ctx.graph != nullptr);
        REQUIRE(ctx.device != nullptr);
        CHECK(ctx.device == app.device());
        CHECK(ctx.extent.width == 64);
        CHECK(ctx.extent.height == 48);

        const render::RGTexture target =
            ctx.graph->create_texture({ctx.extent, rhi::Format::RGBA16Float, "app-frame"});
        const render::RGColorAttachment color{
            target, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.1f, 0.2f, 0.3f, 1.0f}};
        render::RenderGraph::RasterPassDesc desc{};
        desc.colors = {&color, 1};
        ctx.graph->add_raster_pass("app-clear", desc, [](rhi::CommandBuffer&) {});
        ctx.graph->export_texture(target);
    });

    // Three whole-tick frames: the callback runs once per frame (render is frame-rate, not
    // tick-rate — a frame that ran zero ticks would still render), and each execute completes
    // cleanly on the device the app owns.
    const double fd = app.fixed_dt();
    for (int i = 0; i < 3; ++i)
        app.step(fd);
    CHECK(renders == 3);
    CHECK(app.frame_index() == 3);
    CHECK(app.tick_count() == 3);
}

} // namespace
