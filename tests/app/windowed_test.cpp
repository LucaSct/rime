// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.3a — windowed present, and the half of it CI can actually see.
//
// CI has no display, so the interesting assertion here is NOT "a window opened". It is the
// DEGRADATION CONTRACT: asking for a window on a machine that has none must leave a fully working
// headless app rather than a dead one. That is what lets a display-bearing sample be built,
// shipped, and run by the test farm as the same binary — and it is exactly the property that goes
// silently wrong, because the person who wrote the windowed path always has a display.
//
// The presenting path itself is proven two ways elsewhere: `PresentPass` gets a pixel proof in
// tests/render (lavapipe, no window needed), and the end-to-end window is exercised by hand on a
// display-bearing box — recorded in the PR rather than pretended about here.

#include <doctest/doctest.h>

#include <cstdlib>

#include "rime/app/application.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/window.hpp"

using namespace rime;

namespace {

// Force the null window backend for the duration of a case, so the degradation contract is tested
// on EVERY machine and not only on the runners that happen to lack a display. Without this the
// assertion below is vacuous on a developer's desktop — which is precisely where the windowed path
// is written, and precisely why it would rot.
struct ForceHeadless {
    bool previous = platform::headless();

    ForceHeadless() { platform::set_headless(true); }

    ~ForceHeadless() { platform::set_headless(previous); }
};

} // namespace

TEST_CASE("m13.3a: asking for a window without a display leaves a working headless app") {
    const ForceHeadless no_display;

    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.windowed = true;
    cfg.window_title = "rime-app-test";
    cfg.window_size = {320, 240};
    cfg.render_extent = {64, 64};
    cfg.tick_hz = 60.0;

    app::Application app(cfg);

    // `windowed()` reports what ACTUALLY came up, never what was asked for. Asserting the config
    // field instead would be a test that can never fail.
    CHECK_FALSE(app.windowed());
    CHECK(app.window() == nullptr);

    // Either way the loop runs and the simulation ticks. This is the contract that matters: a
    // request for a window is a REQUEST.
    int ticks = 0;
    app.on_fixed_tick([&ticks](ecs::World&, double) { ++ticks; });

    bool rendered = false;
    app.on_render([&rendered](app::FrameContext& ctx) {
        rendered = true;
        // A headless run of a windowed-app callback sets `present` and nothing happens — which is
        // the property that lets one render callback serve both, instead of the sample growing two.
        ctx.present = render::RGTexture{};
        CHECK_FALSE(ctx.present.is_valid());
    });

    for (int i = 0; i < 4; ++i) {
        (void)app.step(1.0 / 60.0);
    }
    CHECK(rendered);
    CHECK(ticks >= 3);
    CHECK(app.frame_index() == 4);
}

TEST_CASE("m13.3a: a windowed request without a GPU is still not fatal") {
    // `windowed` requires `gpu` — presenting means a swapchain and a swapchain means a device. The
    // combination below is a caller mistake, and the response is a warning and a headless app, not
    // a crash: this is the same posture `config.gpu` itself has when no Vulkan device exists.
    app::AppConfig cfg{};
    cfg.gpu = false;
    cfg.windowed = true;

    app::Application app(cfg);
    CHECK_FALSE(app.windowed());
    CHECK(app.window() == nullptr);
    CHECK(app.device() == nullptr);

    int ticks = 0;
    app.on_fixed_tick([&ticks](ecs::World&, double) { ++ticks; });
    (void)app.step(1.0 / 30.0);
    (void)app.step(1.0 / 30.0);
    CHECK(ticks > 0);
}
