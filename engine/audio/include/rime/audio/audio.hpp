// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rime/core/math/vec.hpp"

// engine/audio — the newborn audio seam (M8.4). Today it is one interface and a null backend: the
// smallest thing that lets the rest of the engine (the destruction fan-out first) say "play this
// sound, here, this loud" against a STABLE boundary while the actual mixer is still unwritten. The
// real spatializing/mixing backend is a later track (au1) that replaces NullAudioBackend without
// touching a single call site — the whole point of introducing the interface now. A removable
// feature module (guardrail 2): nothing below it depends on it, and the engine builds with it gone.
namespace rime::audio {

// A sound-asset id. Opaque in v1 — an integer the caller and backend agree on (gameplay hands out
// "impact_concrete = 1" and friends). The asset pipeline will mint real hashed ids later; keeping
// it a bare handle now avoids inventing a sound-asset format before there is a mixer to use it.
using SoundId = std::uint32_t;

// The audio backend interface: the one call the engine makes to produce sound. Virtual dispatch is
// deliberate and fine here — audio events are sparse (an impact, a footstep), not a per-vertex hot
// loop, so the indirection is free relative to the work, and the seam is what buys backend swap.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // Play `sound` as a one-shot at world `position` with linear `gain` (1 = unattenuated, 0 =
    // silent). Fire-and-forget in v1 — no voice handle to stop or re-aim later; au1 introduces
    // voices when a consumer needs to hold one. `position` lets a real backend pan/attenuate by
    // distance; the null backend just records it.
    virtual void play(SoundId sound, core::Vec3 position, float gain) = 0;
};

// The null backend: instead of making sound, it LOGS each play() call. This is what makes audio
// testable and demoable with no device — a headless test (or the M8.6 sample's self-check) asserts
// "the break played exactly these sounds, here, this loud". au1 swaps it for a real mixer.
class NullAudioBackend final : public AudioBackend {
public:
    struct Call {
        SoundId sound = 0;
        core::Vec3 position{0.0f, 0.0f, 0.0f};
        float gain = 0.0f;
    };

    void play(SoundId sound, core::Vec3 position, float gain) override;

    // Every play() so far, in call order.
    [[nodiscard]] std::span<const Call> log() const noexcept;
    void clear() noexcept;

private:
    std::vector<Call> log_;
};

} // namespace rime::audio
