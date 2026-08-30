// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.3c — the device → intent map. Analytic, GPU-free, no world and no window.
//
// Two cases here carry more weight than the rest. `map_frame_input rolls the frame` is the one that
// proves the fold calls Input::new_frame(), without which every edge latches true forever and ESC
// quits the app on every frame after the first. And `the fly basis IS the mover's basis` re-derives
// character.cpp's right/forward vectors independently, for the same reason m13.3a's 37-angle case
// exists: two trig expressions of one angle in two modules, whose disagreement you feel instantly
// and cannot see in any state dump.

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "rime/gameplay/character.hpp"
#include "rime/gameplay/first_person.hpp"
#include "rime/gameplay/input_map.hpp"
#include "rime/gameplay/weapon.hpp"

using namespace rime;
using namespace rime::gameplay;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

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

// The mover's own basis, written out longhand from character.cpp:536-538 rather than called, so
// this file is an independent second derivation and not a tautology.
[[nodiscard]] core::Vec3 mover_right(float yaw) noexcept {
    return {std::cos(yaw), 0.0f, -std::sin(yaw)};
}

[[nodiscard]] core::Vec3 mover_forward(float yaw) noexcept {
    return {-std::sin(yaw), 0.0f, -std::cos(yaw)};
}

} // namespace

TEST_CASE("m13.3c: WASD maps onto the mover's ground-plane axes") {
    platform::Input in;
    FirstPersonView view{};

    const std::vector<platform::Event> w{key_event(platform::Key::W, true)};
    CHECK(map_frame_input(in, w, view).character.move_y == doctest::Approx(1.0f));

    // Release W and press S in the same frame: the fold is state-based, so the order within the
    // frame does not matter and the result is a clean reversal.
    const std::vector<platform::Event> s{key_event(platform::Key::W, false),
                                         key_event(platform::Key::S, true)};
    CHECK(map_frame_input(in, s, view).character.move_y == doctest::Approx(-1.0f));

    const std::vector<platform::Event> d{key_event(platform::Key::S, false),
                                         key_event(platform::Key::D, true)};
    const FrameIntent right = map_frame_input(in, d, view);
    CHECK(right.character.move_x == doctest::Approx(1.0f));
    CHECK(right.character.move_y == doctest::Approx(0.0f));

    const std::vector<platform::Event> a{key_event(platform::Key::D, false),
                                         key_event(platform::Key::A, true)};
    CHECK(map_frame_input(in, a, view).character.move_x == doctest::Approx(-1.0f));
}

TEST_CASE("m13.3c: opposed keys cancel rather than one winning") {
    platform::Input in;
    FirstPersonView view{};
    const std::vector<platform::Event> both{key_event(platform::Key::W, true),
                                            key_event(platform::Key::S, true),
                                            key_event(platform::Key::A, true),
                                            key_event(platform::Key::D, true)};
    const FrameIntent i = map_frame_input(in, both, view);
    CHECK(i.character.move_x == doctest::Approx(0.0f));
    CHECK(i.character.move_y == doctest::Approx(0.0f));
}

TEST_CASE("m13.3c: a diagonal is on the unit disc, not the unit square") {
    platform::Input in;
    FirstPersonView view{};
    const std::vector<platform::Event> wd{key_event(platform::Key::W, true),
                                          key_event(platform::Key::D, true)};
    const FrameIntent i = map_frame_input(in, wd, view);
    const float len = std::sqrt(i.character.move_x * i.character.move_x +
                                i.character.move_y * i.character.move_y);
    // The bug this refuses: (1, 1) is 41% faster than (1, 0), so a player who walks diagonally
    // everywhere outruns one who does not.
    CHECK(len == doctest::Approx(1.0f));
    CHECK(i.character.move_x == doctest::Approx(i.character.move_y));
}

TEST_CASE("m13.3c: map_frame_input rolls the frame, so edges are edges") {
    platform::Input in;
    FirstPersonView view{};

    // ESC goes down and is HELD for three frames. Exactly one of them may report quit. If the fold
    // forgot Input::new_frame(), `pressed` never clears and all three do — which in the sample
    // means an app that exits on the frame after you tap ESC and also on every frame you hold it.
    const std::vector<platform::Event> down{key_event(platform::Key::Escape, true)};
    const std::vector<platform::Event> none{};
    CHECK(map_frame_input(in, down, view).quit);
    CHECK_FALSE(map_frame_input(in, none, view).quit);
    CHECK_FALSE(map_frame_input(in, none, view).quit);

    // Released and pressed again: a second edge, so a second quit.
    const std::vector<platform::Event> up{key_event(platform::Key::Escape, false)};
    CHECK_FALSE(map_frame_input(in, up, view).quit);
    CHECK(map_frame_input(in, down, view).quit);
}

TEST_CASE("m13.3c: held is a level and pressed is an edge") {
    platform::Input in;
    FirstPersonView view{};
    const std::vector<platform::Event> fire{button_event(platform::MouseButton::Left, true)};
    const std::vector<platform::Event> none{};

    const FrameIntent first = map_frame_input(in, fire, view);
    CHECK((first.character.held & kActionFire) != 0u);
    CHECK((first.character.pressed & kActionFire) != 0u);

    // Still held on the next frame; no longer a fresh press. That difference is what stops one
    // click from firing every frame the button stays down.
    const FrameIntent second = map_frame_input(in, none, view);
    CHECK((second.character.held & kActionFire) != 0u);
    CHECK((second.character.pressed & kActionFire) == 0u);

    const std::vector<platform::Event> jump{key_event(platform::Key::Space, true)};
    const FrameIntent j = map_frame_input(in, jump, view);
    CHECK((j.character.held & kActionJump) != 0u);
    CHECK((j.character.pressed & kActionJump) != 0u);
}

TEST_CASE("m13.3c: look is gated on the drag button while pointer lock does not exist") {
    platform::Input in;
    FirstPersonView view{};

    // No button held: the pointer is the desktop's, and moving it must not steer the camera.
    const std::vector<platform::Event> drift{motion_event(100.0f, 40.0f)};
    (void)map_frame_input(in, drift, view);
    CHECK(view.yaw == doctest::Approx(0.0f));
    CHECK(view.pitch == doctest::Approx(0.0f));

    const std::vector<platform::Event> drag{button_event(platform::MouseButton::Right, true),
                                            motion_event(100.0f, 40.0f)};
    (void)map_frame_input(in, drag, view);
    CHECK(view.yaw < 0.0f);   // rightward pointer motion turns right, and right decreases yaw
    CHECK(view.pitch < 0.0f); // downward pointer motion looks down

    // The gate is a binding, not a law: a game that has capture turns it off.
    InputBindings free_look{};
    free_look.look_requires_drag = false;
    FirstPersonView v2{};
    platform::Input in2;
    (void)map_frame_input(in2, drift, v2, free_look);
    CHECK(v2.yaw < 0.0f);
}

TEST_CASE("m13.3c: the intent carries the view's CURRENT angle to the mover") {
    // The handoff m13.3a left unmade. If this assignment is missing or reads a stale view, the
    // character walks along the angle it was looking at last frame — which reads as the walk
    // "curving" out of fast turns and is invisible in any single state dump.
    platform::Input in;
    FirstPersonView view{};
    const std::vector<platform::Event> drag{button_event(platform::MouseButton::Right, true),
                                            motion_event(250.0f, -60.0f)};
    const FrameIntent i = map_frame_input(in, drag, view);
    CHECK(i.character.yaw == doctest::Approx(view.yaw));
    CHECK(i.character.pitch == doctest::Approx(view.pitch));
    CHECK(view.yaw !=
          doctest::Approx(0.0f)); // and the frame really did turn, so this is not vacuous
}

TEST_CASE("m13.3c: the fly basis IS the mover's basis, across a full turn") {
    // Twenty-four yaws around the circle. `fly_step` builds `right` in closed form from yaw; the
    // mover builds it from the same angle in another translation unit. They must agree exactly, or
    // strafing means different things depending on which one is driving.
    for (int i = 0; i < 24; ++i) {
        const float yaw = -kPi + (2.0f * kPi * static_cast<float>(i)) / 24.0f;
        FirstPersonView view{};
        view.yaw = yaw;

        FrameIntent strafe{};
        strafe.character.move_x = 1.0f;
        const core::Vec3 r = fly_step(view, strafe, {}, 1.0f, 1.0f);
        const core::Vec3 want_r = mover_right(yaw);
        CHECK(r.x == doctest::Approx(want_r.x).epsilon(1e-5));
        CHECK(r.y == doctest::Approx(want_r.y).epsilon(1e-5));
        CHECK(r.z == doctest::Approx(want_r.z).epsilon(1e-5));

        FrameIntent ahead{};
        ahead.character.move_y = 1.0f;
        const core::Vec3 f = fly_step(view, ahead, {}, 1.0f, 1.0f);
        const core::Vec3 want_f = mover_forward(yaw);
        CHECK(f.x == doctest::Approx(want_f.x).epsilon(1e-5));
        CHECK(f.y == doctest::Approx(want_f.y).epsilon(1e-5));
        CHECK(f.z == doctest::Approx(want_f.z).epsilon(1e-5));
    }
}

TEST_CASE("m13.3c: flying uses the pitched forward, walking does not") {
    FirstPersonView view{};
    view.pitch = 0.5f; // looking up

    FrameIntent ahead{};
    ahead.character.move_y = 1.0f;
    const core::Vec3 p = fly_step(view, ahead, {}, 1.0f, 1.0f);
    // A walker would stay at y = 0 here. A free camera climbs — that IS the difference between the
    // two, and asserting it stops someone "fixing" fly_step to flatten like the mover.
    CHECK(p.y == doctest::Approx(std::sin(0.5f)).epsilon(1e-5));

    // Vertical is world up regardless of where the view points, so rise always rises.
    FirstPersonView tilted{};
    tilted.pitch = -1.0f;
    tilted.yaw = 2.0f;
    FrameIntent rise{};
    rise.vertical = 1.0f;
    const core::Vec3 up = fly_step(tilted, rise, {}, 1.0f, 1.0f);
    CHECK(up.x == doctest::Approx(0.0f));
    CHECK(up.y == doctest::Approx(1.0f));
    CHECK(up.z == doctest::Approx(0.0f));
}

TEST_CASE("m13.3c: three axes at once is not √3 times faster") {
    FirstPersonView view{};
    FrameIntent all{};
    all.character.move_x = 1.0f;
    all.character.move_y = 1.0f;
    all.vertical = 1.0f;
    const core::Vec3 p = fly_step(view, all, {}, 10.0f, 1.0f);
    CHECK(core::length(p) == doctest::Approx(10.0f).epsilon(1e-5));
}

TEST_CASE("m13.3c: FlyCamera integrates, latches quit, and IS its eye") {
    FlyCamera cam;
    cam.speed = 2.0f;
    cam.position = {0.0f, 1.0f, 0.0f};

    const std::vector<platform::Event> w{key_event(platform::Key::W, true)};
    cam.update(w, 0.5f); // 2 m/s for half a second, forward = −Z
    CHECK(cam.position.z == doctest::Approx(-1.0f));
    CHECK(cam.position.y == doctest::Approx(1.0f));

    // eye_height is zero for a free camera, so the posed transform sits exactly where the caller
    // put it. The 0.6 m default is a CHARACTER's eye above its capsule centre.
    CHECK(cam.transform().translation.y == doctest::Approx(cam.position.y));

    // Boost multiplies, and only while held.
    const std::vector<platform::Event> boost{key_event(platform::Key::LeftShift, true)};
    cam.boost_scale = 3.0f;
    const float before = cam.position.z;
    cam.update(boost, 0.5f);
    CHECK(cam.position.z == doctest::Approx(before - 3.0f)); // 2 × 3 × 0.5

    CHECK_FALSE(cam.quit_requested());
    const std::vector<platform::Event> esc{key_event(platform::Key::Escape, true)};
    cam.update(esc, 0.016f);
    CHECK(cam.quit_requested());
    // Latched: a caller that checks once per second must not miss a tap that happened between.
    const std::vector<platform::Event> quiet{};
    cam.update(quiet, 0.016f);
    CHECK(cam.quit_requested());
}

// m15.5. The one-line policy that decides a control scheme, tested because the tempting way to
// write it at the call site — comparing against the mode you REQUESTED — is a tautology that always
// says "locked" and always ships free-look on a cursor that is not locked.
TEST_CASE("m15.5: hold-to-look is required unless the cursor is actually locked") {
    CHECK_FALSE(look_requires_drag_for(platform::CursorMode::Locked));
    CHECK(look_requires_drag_for(platform::CursorMode::Normal));
    // Hidden is NOT locked: the cursor is invisible but still moving freely, so it still walks off
    // the window and the camera still stops at the edge. Treating "we hid it" as "we captured it"
    // is the subtle version of the same mistake.
    CHECK(look_requires_drag_for(platform::CursorMode::Hidden));
}
