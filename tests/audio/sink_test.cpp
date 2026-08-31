// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "rime/audio/sink.hpp"

// The platform sink proof. Whatever backend CMake compiled in — ALSA on Linux, CoreAudio on macOS,
// the "no device" stub elsewhere — the contract in sink.hpp is the same, and this asserts the part
// of it that a caller actually depends on: THE SINK IS THE CLOCK. `write` blocks until the device
// has taken the frames, which is what paces a playback loop. A sink that accepted everything
// instantly would let the mixer free-run and the proof would be measuring nothing.
//
// WHY THIS SKIPS RATHER THAN FAILS WHEN THERE IS NO DEVICE. sink.hpp is explicit that a null return
// is an ordinary answer, not an error: no CI runner has a sound card, and some have no ALSA headers
// at build time either. So "no device" is a skip, and — importantly — so is a device that opens but
// never consumes, which is what a headless macOS runner with a nominal default output looks like.
// Neither case can redden CI; both leave the assertion below for a machine that really has audio.
using namespace rime;

TEST_CASE("audio sink: the platform sink is the playback clock") {
    constexpr std::uint32_t kRate = 48'000;
    auto sink = audio::open_audio_sink(kRate);
    if (sink == nullptr) {
        MESSAGE("no audio device or no backend compiled in — skipping the sink proof");
        return;
    }

    REQUIRE(sink->backend_name() != nullptr);
    MESSAGE("audio sink backend: " << std::string(sink->backend_name()));
    CHECK(sink->underruns() == 0); // nothing has been asked of it yet

    // 10 ms blocks of silence, 25 of them: a quarter second of audio.
    constexpr int kBlocks = 25;
    constexpr double kBlockSeconds = 0.01;
    std::vector<float> block(static_cast<std::size_t>(kRate * kBlockSeconds) * 2u, 0.0f);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kBlocks; ++i) {
        if (!sink->write(block)) {
            MESSAGE("the device stopped consuming — skipping the pacing assertion");
            return;
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);

    // The floor is deliberately loose. The sink's own buffer swallows its depth (~40 ms) before it
    // ever blocks, a device may run at a rate other than the one we asked for, and a loaded test
    // box schedules us late. Half the audio's real duration still cannot be reached by a sink that
    // is not waiting on hardware at all, which is the only thing this needs to distinguish.
    MESSAGE("wrote " << kBlocks * kBlockSeconds << " s of audio in " << elapsed.count()
                     << " s, underruns " << sink->underruns());
    CHECK(elapsed.count() >= kBlocks * kBlockSeconds * 0.5);
}
