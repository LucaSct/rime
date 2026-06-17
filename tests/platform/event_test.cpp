// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the M2.2a event queue: post_event/poll_event form a FIFO that drains to empty. The
// queue is the seam every backend pushes through, so exercising it directly (no window needed)
// pins down the ordering contract the input layer (M2.3) will rely on.
#include <doctest/doctest.h>

#include "rime/platform/event.hpp"

using namespace rime::platform;

TEST_CASE("event queue is FIFO and reports empty") {
    // Other cases share the process-global queue; start from a known-empty state.
    Event drain{};
    while (poll_event(drain)) {
    }

    Event a{};
    a.type = EventType::KeyDown;
    a.key.key = Key::A;

    Event b{};
    b.type = EventType::MouseButton;
    b.button.button = MouseButton::Left;
    b.button.down = true;

    post_event(a);
    post_event(b);

    Event out{};
    REQUIRE(poll_event(out));
    CHECK(out.type == EventType::KeyDown);
    CHECK(out.key.key == Key::A);

    REQUIRE(poll_event(out));
    CHECK(out.type == EventType::MouseButton);
    CHECK(out.button.button == MouseButton::Left);
    CHECK(out.button.down);

    CHECK_FALSE(poll_event(out)); // drained
}
