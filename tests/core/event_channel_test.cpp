// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <string>

#include "rime/core/containers/event_channel.hpp"

// EventChannel<T> (M8.4): the double-buffered "push during the tick, publish once, read the batch
// after" queue. These pin its contract — the destruction fan-out (and later fx/lighting/audio)
// leans on every clause: order is preserved, publish() closes the frame, and the previous frame is
// cleared.
using namespace rime;

TEST_CASE("EventChannel: pushes are invisible until published, then a stable batch") {
    core::EventChannel<int> ch;
    CHECK(ch.view().empty()); // nothing published yet
    CHECK(ch.pending() == 0);

    ch.push(10);
    ch.push(20);
    CHECK(ch.pending() == 2); // queued…
    CHECK(ch.view().empty()); // …but not yet visible (still the empty prior frame)

    ch.publish();
    CHECK(ch.pending() == 0);
    REQUIRE(ch.view().size() == 2);
    CHECK(ch.view()[0] == 10); // order preserved verbatim
    CHECK(ch.view()[1] == 20);
}

TEST_CASE("EventChannel: publish clears the previous frame (clean on a quiet tick)") {
    core::EventChannel<int> ch;
    ch.push(1);
    ch.push(2);
    ch.publish();
    REQUIRE(ch.view().size() == 2);

    // A tick with no pushes: publish() swaps in the (empty) write buffer — last frame is gone.
    ch.publish();
    CHECK(ch.view().empty());

    // A new frame after a quiet one works normally.
    ch.push(3);
    ch.publish();
    REQUIRE(ch.view().size() == 1);
    CHECK(ch.view()[0] == 3);
}

TEST_CASE("EventChannel: the published view survives the next round of pushes until publish()") {
    core::EventChannel<int> ch;
    ch.push(100);
    ch.publish();
    const std::span<const int> frame = ch.view();
    REQUIRE(frame.size() == 1);

    // Pushing the next frame does NOT disturb the currently-published one (double buffer).
    ch.push(200);
    ch.push(300);
    CHECK(frame[0] == 100);
    CHECK(ch.view().size() == 1); // still last frame
    ch.publish();
    REQUIRE(ch.view().size() == 2); // now the new frame
}

TEST_CASE("EventChannel: works with a move-only-ish payload and clear() resets everything") {
    core::EventChannel<std::string> ch;
    ch.push(std::string("break"));
    std::string moved = "settle";
    ch.push(std::move(moved));
    ch.publish();
    REQUIRE(ch.view().size() == 2);
    CHECK(ch.view()[0] == "break");
    CHECK(ch.view()[1] == "settle");

    ch.clear();
    CHECK(ch.view().empty());
    CHECK(ch.pending() == 0);
}
