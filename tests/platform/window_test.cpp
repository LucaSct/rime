// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the M2.2a window seam via the headless null backend: create/size/close a window and
// drive the run-loop predicate (pump_events) with no window server. This is the test the real
// Cocoa/Win32/X11/Wayland backends are held to behaviourally, minus the pixels.
#include <doctest/doctest.h>

#include "rime/platform/event.hpp"
#include "rime/platform/init.hpp"
#include "rime/platform/window.hpp"

using namespace rime::platform;

TEST_CASE("null window: lifecycle, sizing, and close") {
    set_headless(true); // force the null backend regardless of OS
    REQUIRE(init());

    auto win = create_window(WindowDesc{.title = "test", .width = 640, .height = 480});
    REQUIRE(win != nullptr);
    CHECK(win->native_handle().system == WindowSystem::Null);
    CHECK(win->framebuffer_size().width == 640);
    CHECK(win->framebuffer_size().height == 480);
    CHECK(win->content_scale() == doctest::Approx(1.0f));
    CHECK(win->id().is_valid());

    win->set_size(Extent2D{800, 600});
    CHECK(win->framebuffer_size().width == 800);
    CHECK(win->framebuffer_size().height == 600);

    CHECK_FALSE(win->should_close());
    CHECK(pump_events()); // nothing has asked to quit yet

    win->request_close();
    CHECK(win->should_close());
    CHECK_FALSE(pump_events()); // a close was requested -> loop predicate is now false

    // The close also surfaced as an event for the app to observe.
    Event e{};
    bool saw_close = false;
    while (poll_event(e)) {
        if (e.type == EventType::WindowClose) {
            saw_close = true;
        }
    }
    CHECK(saw_close);

    shutdown(); // clears the queue and resets the quit flag for the next case
}

// m15.5. The contract that matters about pointer capture is not "the mode was stored" — it is that
// ASKING IS NOT GETTING. `set_cursor_mode` returns the mode actually in effect, which the caller
// must read; a Wayland compositor may not advertise pointer constraints, another X11 client may own
// the pointer, and the headless backend has no pointer at all. Every one of those is a real
// deployment, and a caller that assumed its request was granted puts the player in free-look with a
// cursor still free to walk off the window.
//
// The null backend is the honest worst case and so the best place to pin this: it refuses
// everything, which means the fallback path is exercised on every CI OS on every run rather than
// only on the machines where refusal is hard to reproduce.
TEST_CASE("null window: a cursor-mode request is answered, not echoed") {
    set_headless(true);
    REQUIRE(init());
    auto win = create_window(WindowDesc{.title = "cursor", .width = 320, .height = 240});
    REQUIRE(win != nullptr);

    CHECK(win->cursor_mode() == CursorMode::Normal); // the resting state

    // Not Locked, and that is the assertion. A backend that echoed the request would pass every
    // weaker check ("it returned something", "the mode is one of the enumerators") and fail this.
    CHECK(win->set_cursor_mode(CursorMode::Locked) == CursorMode::Normal);
    CHECK(win->cursor_mode() == CursorMode::Normal);

    CHECK(win->set_cursor_mode(CursorMode::Hidden) == CursorMode::Normal);
    CHECK(win->cursor_mode() == CursorMode::Normal);

    // Asking for what it already has is still answered truthfully.
    CHECK(win->set_cursor_mode(CursorMode::Normal) == CursorMode::Normal);

    shutdown();
}
