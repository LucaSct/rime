// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.3c — THE HANDOFF. Not "does FirstPersonView compute the right vector" (m13.3a proved that
// across 37 angles) and not "does map_frame_input fold keys correctly" (tests/gameplay proves that)
// — but the join: an event posted to an Application reaches a camera and MOVES it, and ESC ends the
// loop.
//
// This file exists because m13.3a shipped green with every part built and none of them connected.
// Its proof asserted a value; nothing asserted that anything called the code under test. The repo's
// own note on that pattern (docs: assert the handoff, not the value) has now been earned four
// times, so the join gets a test of its own.
//
// GPU-free and windowless on purpose. Application runs the render callback with a null graph when
// it owns no device, and post_input() feeds the SAME snapshot the OS pump fills — which is the
// whole reason ADR-0023 §5 routed both through one path. So this runs on every CI OS and under both
// sanitizers, and it is meaningful about the windowed build.

#include <doctest/doctest.h>

#include <vector>

#include "rime/app/application.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/input_map.hpp"
#include "rime/platform/event.hpp"

using namespace rime;
using namespace rime::app;

namespace {

platform::Event key_event(platform::Key k, bool down) {
    platform::Event e{};
    e.type = down ? platform::EventType::KeyDown : platform::EventType::KeyUp;
    e.key.key = k;
    e.key.repeat = false;
    return e;
}

platform::Event button_event(platform::MouseButton b, bool down) {
    platform::Event e{};
    e.type = platform::EventType::MouseButton;
    e.button.button = b;
    e.button.down = down;
    return e;
}

platform::Event motion_event(float dx, float dy) {
    platform::Event e{};
    e.type = platform::EventType::MouseMove;
    e.mouse_move.dx = dx;
    e.mouse_move.dy = dy;
    return e;
}

// The wiring under test, in the shape a sample uses it: a FlyCamera driven from the frame's input
// snapshot, posing a camera entity. `07-first-light`'s run_windowed does exactly this.
struct DrivenCamera {
    gameplay::FlyCamera cam;
    ecs::Entity entity{};
    int frames = 0;

    // A backstop, because one case below drives the UNBOUNDED run() loop. If the quit path is
    // broken, that loop never ends — and a test that hangs CI is strictly worse than one that
    // fails, since a hang says nothing about what broke and blocks every other job behind it. The
    // guard turns "ESC does not work" into an assertion failure with a frame count attached.
    static constexpr int kRunaway = 600;

    void attach(Application& app) {
        entity = app.world().spawn_with(ecs::WorldTransform{});
        app.on_render([this, &app](FrameContext& ctx) {
            // FRAME time, from the context (m13.3c added the field): a free camera is presentation,
            // so it must not run on the fixed tick. Reading it here also means this proof exercises
            // the plumbing of `frame_dt` rather than a value the test made up.
            cam.update(ctx.input, static_cast<float>(ctx.frame_dt));
            app.world().get<ecs::WorldTransform>(entity)->value = cam.transform();
            if (cam.quit_requested()) {
                app.request_quit();
            }
            if (++frames >= kRunaway) {
                app.request_quit();
            }
        });
    }

    [[nodiscard]] core::Vec3 posed(Application& app) const {
        return app.world().get<ecs::WorldTransform>(entity)->value.translation;
    }
};

} // namespace

TEST_CASE("m13.3c: a key posted to the app moves the camera it is wired to") {
    Application app{}; // headless, GPU-free — the callback still runs
    DrivenCamera dc;
    dc.cam.speed = 60.0f; // 60 m/s at a 1/60 s frame = exactly 1 m per step, so the maths is plain
    dc.attach(app);

    app.step(1.0 / 60.0); // a frame with no input: establishes the baseline pose
    const core::Vec3 start = dc.posed(app);
    CHECK(dc.frames == 1);

    app.post_input(key_event(platform::Key::W, true));
    app.step(1.0 / 60.0);
    const core::Vec3 after = dc.posed(app);

    // Forward is −Z at yaw 0.
    CHECK(after.z == doctest::Approx(start.z - 1.0f));
    CHECK(after.x == doctest::Approx(start.x));

    // Release it and the camera stops. A held-key map that never sees the KeyUp keeps walking
    // forever — the failure mode of reading edges where a level was meant.
    app.post_input(key_event(platform::Key::W, false));
    app.step(1.0 / 60.0);
    const core::Vec3 stopped = dc.posed(app);
    app.step(1.0 / 60.0);
    CHECK(dc.posed(app).z == doctest::Approx(stopped.z));
}

TEST_CASE("m13.3c: a dragged pointer posted to the app turns the camera it is wired to") {
    Application app{};
    DrivenCamera dc;
    dc.attach(app);

    app.step(1.0 / 60.0);
    const float yaw0 = dc.cam.view.yaw;

    // Motion alone must not steer — there is no pointer capture, so an untouched cursor crossing
    // the window is not an instruction.
    app.post_input(motion_event(120.0f, 0.0f));
    app.step(1.0 / 60.0);
    CHECK(dc.cam.view.yaw == doctest::Approx(yaw0));

    app.post_input(button_event(platform::MouseButton::Right, true));
    app.post_input(motion_event(120.0f, 0.0f));
    app.step(1.0 / 60.0);
    CHECK(dc.cam.view.yaw < yaw0);

    // And the turn reaches the POSE, not just the view struct: walking forward now goes somewhere
    // else than −Z, which is the property a player actually feels.
    const core::Vec3 before = dc.posed(app);
    app.post_input(key_event(platform::Key::W, true));
    app.step(1.0 / 60.0);
    const core::Vec3 moved = dc.posed(app);
    CHECK(moved.x != doctest::Approx(before.x));
}

TEST_CASE("m13.3c: ESC posted to the app ends run()") {
    // The promise 07-first-light printed for two milestones and never kept. `run()` is the
    // unbounded loop a windowed session actually uses, so the quit path is proved through THAT and
    // not through a bounded run_frames() that would have ended anyway.
    Application app{};
    DrivenCamera dc;
    dc.attach(app);

    // Post ESC up front: run() snapshots pending input at its first frame edge, so the very first
    // callback sees it and requests quit.
    app.post_input(key_event(platform::Key::Escape, true));
    const std::uint64_t frames = app.run();

    CHECK(app.quit_requested());
    CHECK(frames >= 1);
    // It ended because of ESC, not because the runaway guard tripped. Without this the case would
    // pass on a completely dead input path.
    CHECK(dc.frames < DrivenCamera::kRunaway);
}

TEST_CASE("m13.3c: a held ESC does not re-quit, and an unwired app is unaffected") {
    // The edge/level distinction, at the app level: holding ESC for many frames is one quit.
    Application app{};
    DrivenCamera dc;
    dc.attach(app);

    app.post_input(key_event(platform::Key::Escape, true));
    app.step(1.0 / 60.0);
    CHECK(app.quit_requested());
    const int at_quit = dc.frames;

    // The loop is the caller's here, so it keeps stepping; the point is that nothing else changes.
    app.step(1.0 / 60.0);
    CHECK(dc.frames == at_quit + 1);
    CHECK(dc.cam.quit_requested());
}
