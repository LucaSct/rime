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
