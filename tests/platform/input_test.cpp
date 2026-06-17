// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the M2.3 Input layer: edge detection (pressed/released vs held) and the per-frame
// accumulators (mouse delta, wheel, text) behave correctly when fed synthetic events. No window or
// OS is involved — Input is pure state over the event stream.
#include <doctest/doctest.h>

#include <cstring>

#include "rime/platform/event.hpp"
#include "rime/platform/input.hpp"

using namespace rime::platform;

namespace {
Event key_event(EventType type, Key k) {
    Event e{};
    e.type = type;
    e.key.key = k;
    return e;
}
} // namespace

TEST_CASE("Input: key edge detection across frames") {
    Input in;

    // Frame 1: press W.
    in.new_frame();
    in.process(key_event(EventType::KeyDown, Key::W));
    CHECK(in.key_down(Key::W));
    CHECK(in.key_pressed(Key::W));
    CHECK_FALSE(in.key_released(Key::W));

    // Frame 2: still held, no new event -> down but not a fresh press.
    in.new_frame();
    CHECK(in.key_down(Key::W));
    CHECK_FALSE(in.key_pressed(Key::W));

    // Frame 3: release.
    in.new_frame();
    in.process(key_event(EventType::KeyUp, Key::W));
    CHECK_FALSE(in.key_down(Key::W));
    CHECK(in.key_released(Key::W));

    // Unmapped keys are simply never down (and indexing is in-bounds).
    CHECK_FALSE(in.key_down(Key::Unknown));
}

TEST_CASE("Input: mouse buttons, motion, wheel, and typed text") {
    Input in;

    in.new_frame();
    Event b{};
    b.type = EventType::MouseButton;
    b.button.button = MouseButton::Left;
    b.button.down = true;
    in.process(b);
    CHECK(in.mouse_down(MouseButton::Left));
    CHECK(in.mouse_pressed(MouseButton::Left));

    Event m{};
    m.type = EventType::MouseMove;
    m.mouse_move.x = 10.0f;
    m.mouse_move.y = 20.0f;
    m.mouse_move.dx = 3.0f;
    m.mouse_move.dy = -2.0f;
    in.process(m);
    in.process(m); // deltas accumulate within a frame
    CHECK(in.mouse_x() == doctest::Approx(10.0f));
    CHECK(in.mouse_y() == doctest::Approx(20.0f));
    CHECK(in.mouse_dx() == doctest::Approx(6.0f));
    CHECK(in.mouse_dy() == doctest::Approx(-4.0f));

    Event w{};
    w.type = EventType::MouseWheel;
    w.wheel.dy = 1.5f;
    in.process(w);
    CHECK(in.wheel_y() == doctest::Approx(1.5f));

    Event t{};
    t.type = EventType::TextInput;
    std::strcpy(t.text.utf8, "x");
    in.process(t);
    CHECK(in.text() == "x");

    // new_frame clears the per-frame accumulators but keeps held state.
    in.new_frame();
    CHECK(in.mouse_dx() == doctest::Approx(0.0f));
    CHECK(in.wheel_y() == doctest::Approx(0.0f));
    CHECK(in.text().empty());
    CHECK(in.mouse_down(MouseButton::Left));          // still held across the frame
    CHECK_FALSE(in.mouse_pressed(MouseButton::Left)); // but not a fresh press
}
