// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "rime/audio/audio.hpp"

// M8.4 proof for the audio seam: the null backend logs what it was asked to play, so a headless
// test (and the M8.6 sample's self-check) can assert a break made the right sounds without a device
// — and the engine has a stable AudioBackend to call while the real mixer (au1) is still unwritten.
using namespace rime;

TEST_CASE("M8.4 audio: the null backend logs each play() faithfully") {
    audio::NullAudioBackend backend;
    CHECK(backend.log().empty());

    backend.play(7, {1.0f, 2.0f, 3.0f}, 0.5f);
    backend.play(9, {-4.0f, 0.0f, 0.0f}, 1.0f);

    REQUIRE(backend.log().size() == 2);
    CHECK(backend.log()[0].sound == 7);
    CHECK(backend.log()[0].position.x == doctest::Approx(1.0f));
    CHECK(backend.log()[0].position.y == doctest::Approx(2.0f));
    CHECK(backend.log()[0].position.z == doctest::Approx(3.0f));
    CHECK(backend.log()[0].gain == doctest::Approx(0.5f));
    CHECK(backend.log()[1].sound == 9);
    CHECK(backend.log()[1].gain == doctest::Approx(1.0f));

    backend.clear();
    CHECK(backend.log().empty());
}

TEST_CASE("M8.4 audio: callable through the AudioBackend interface (the swap seam)") {
    // A consumer holds an AudioBackend& and never knows which backend it is — the whole point of
    // the interface (au1 slots a real mixer in behind it). Prove the virtual call routes to the
    // null log.
    audio::NullAudioBackend null;
    audio::AudioBackend& backend = null;
    backend.play(3, {0.0f, 0.0f, 0.0f}, 0.25f);
    REQUIRE(null.log().size() == 1);
    CHECK(null.log()[0].sound == 3);
    CHECK(null.log()[0].gain == doctest::Approx(0.25f));
}
