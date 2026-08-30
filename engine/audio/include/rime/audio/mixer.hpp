// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/audio/audio.hpp"
#include "rime/audio/bank.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"

// The engine mixer (m13.4, ADR-0035's audio v1): the real backend behind M8.4's `AudioBackend`
// seam, which has stood since M8.4 with nothing but a logger behind it.
//
// WHAT IT IS. Voices, distance attenuation, constant-power stereo panning, and a deterministic
// float mixdown. What it is NOT, deliberately: no occlusion, no reverb, no HRTF, no doppler, no
// streaming, no voice handles you can stop or re-aim. Every one of those is a real feature and none
// of them is what "the block should make a noise when it falls over" needs.
//
// DETERMINISM IS THE PROOF SURFACE, and it is why this is testable at all. CI is deaf: no runner
// has a sound device, and a test that opened one would be a test that skipped. So the mixer's
// contract is that the same play calls interleaved with the same renders produce **bit-identical
// samples**, and the proof renders to a buffer and asserts on it. That is a stronger claim than
// "it made a sound" and it is available on every machine.
//
// ONE TIMING CONSEQUENCE, stated rather than discovered: a voice starts at the beginning of the
// next rendered block, so timing resolution is the block size. At the 1024-frame blocks a sink
// wants that is ~21 ms — inaudible as jitter for impact audio, and it is what keeps `play()`
// callable from a simulation tick without the mixer needing a clock of its own.
namespace rime::audio {

struct Listener {
    core::Vec3 position{0.0f, 0.0f, 0.0f};
    // Orientation follows the engine-wide convention: forward is the rotation applied to −Z, right
    // is +X. The same rule `render::Camera` and `gameplay::FirstPersonView` use, so pointing the
    // listener is pointing the camera and the two cannot disagree about which way is left.
    core::Quat orientation = core::quat_identity();
};

struct MixerConfig {
    std::uint32_t sample_rate = kSampleRate;

    // The cap. Past it a new voice STEALS the quietest one rather than being dropped: a loud impact
    // arriving during a wall of settling rubble must be heard, and the alternative — refusing the
    // new voice — makes the biggest moments the ones most likely to go silent.
    std::uint32_t max_voices = 48;

    // Distance model. Inside `reference_distance` there is no attenuation; past it the level falls
    // as ref/d, tapered linearly to EXACTLY zero at `max_distance`. The taper matters: a pure
    // inverse law never reaches silence, so distant voices accumulate forever and every one of them
    // costs a voice slot that a close sound wanted.
    float reference_distance = 2.0f;
    float max_distance = 80.0f;

    // MEASURED, not guessed. The first value here was 0.7, and the first real event load — the
    // m8.6 wall's 43 voices from 18 destruction events — peaked at 1.84 and clipped 222 samples.
    // 0.35 clears that case with headroom. There is no gain that cannot be overrun by a dense
    // enough scene, which is exactly why `MixStats::clipped_samples` exists: it is the signal to
    // lower this (or, when a scene really needs it, to add the limiter v1 deliberately does not
    // have).
    float master_gain = 0.35f;
};

// What a render did. As everywhere else in this engine, each counter exists because the failure it
// names is otherwise silent — and audio is the worst medium for silent failure, because nobody can
// see it and CI cannot hear it.
struct MixStats {
    std::uint64_t frames_rendered = 0;
    std::size_t voices_active = 0; // still sounding at the end of the block
    std::uint64_t voices_started = 0;
    std::uint64_t voices_stolen = 0;   // evicted by the cap to make room
    std::uint64_t voices_unknown = 0;  // play() named a sound the bank does not have
    std::uint64_t clipped_samples = 0; // hit the [-1, 1] rail — the mix is too hot
    float peak = 0.0f;                 // largest absolute sample before clamping
};

class Mixer final : public AudioBackend {
public:
    Mixer(SoundBank bank, const MixerConfig& config = {});

    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // AudioBackend. Starts a voice at the next block boundary. Cheap and allocation-free in the
    // steady state — safe to call from a simulation tick, which is where destruction events arrive.
    void play(SoundId sound, core::Vec3 position, float gain) override;

    void set_listener(const Listener& listener) noexcept { listener_ = listener; }

    [[nodiscard]] const Listener& listener() const noexcept { return listener_; }

    // Render interleaved STEREO into `out` (its size must be even; frames = size/2) and advance the
    // clock. Fills with silence where nothing is sounding, so a caller can hand this straight to a
    // sink without checking.
    void render(std::span<float> out);

    [[nodiscard]] const MixStats& stats() const noexcept { return stats_; }

    void reset_stats() noexcept;

    [[nodiscard]] std::size_t voice_count() const noexcept { return voices_.size(); }

private:
    struct Voice {
        SoundId sound = 0;
        std::size_t cursor = 0; // next sample index into the bank buffer
        core::Vec3 position{0.0f, 0.0f, 0.0f};
        float gain = 1.0f;
        // Per-channel gains are held across blocks and RAMPED to their new targets inside each
        // block. Recomputing them per block and applying them as a step would click audibly every
        // time the listener turned — the classic zipper artefact — and interpolating is the whole
        // fix.
        float left = 0.0f;
        float right = 0.0f;
        bool started = false; // false until the first block, so gains start AT target, not from 0
    };

    [[nodiscard]] float attenuation(float distance) const noexcept;
    void channel_gains(const Voice& v, float& left, float& right) const noexcept;

    SoundBank bank_;
    MixerConfig config_;
    Listener listener_;
    std::vector<Voice> voices_;
    MixStats stats_;
};

} // namespace rime::audio
