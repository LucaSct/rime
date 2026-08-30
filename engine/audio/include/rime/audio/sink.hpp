// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <memory>
#include <span>

// The audio SINK (m13.4): where mixed samples go to become sound.
//
// ADR-0035 asked for "a Linux sink first (the demo machine; power beats portability, and CI is deaf
// either way)", and both halves of that parenthesis shape this file.
//
// **Power beats portability** is why ALSA and not a cross-platform wrapper: the demo machine runs
// Linux, ALSA is what is actually underneath PulseAudio and PipeWire on it, and a portability layer
// bought now would be a dependency plus an abstraction serving exactly one implementation.
//
// **CI is deaf** is why `open_audio_sink` is allowed to fail and why failing is NOT an error. No
// runner has a sound device; some have no ALSA headers at build time either. A sink that could not
// be opened returns null, the caller mixes into a buffer and writes a WAV instead, and everything
// stays green. This is the same posture the windowed present path takes toward a missing display
// (m13.3a) — a request, not a requirement — and it is the only posture that lets one binary serve
// both the workstation and the test farm.
namespace rime::audio {

class AudioSink {
public:
    virtual ~AudioSink() = default;

    // Push interleaved stereo float frames. Blocks until the device has taken them, which is what
    // paces a playback loop — the sink IS the clock. Returns false on an unrecoverable error; an
    // underrun is recovered internally and reported through `underruns()`.
    [[nodiscard]] virtual bool write(std::span<const float> interleaved) = 0;

    // Underruns since open. A number worth surfacing rather than hiding: it is the difference
    // between "the audio is choppy" and "the mixer is being called too late", and only the sink
    // can tell.
    [[nodiscard]] virtual std::uint64_t underruns() const noexcept = 0;

    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
};

// Open the platform's default stereo sink at `sample_rate`. Null when there is no device, no
// backend compiled in, or no permission — all of which are ordinary, none of which are errors.
[[nodiscard]] std::unique_ptr<AudioSink> open_audio_sink(std::uint32_t sample_rate);

} // namespace rime::audio
