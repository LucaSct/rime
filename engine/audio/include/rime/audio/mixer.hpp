// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/audio/audio.hpp"
#include "rime/audio/bank.hpp"
#include "rime/audio/reverb.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"

// The engine mixer (m13.4, ADR-0035's audio v1): the real backend behind M8.4's `AudioBackend`
// seam, which has stood since M8.4 with nothing but a logger behind it.
//
// WHAT IT IS. Voices, distance attenuation, constant-power stereo panning, a shared reverb send,
// and a deterministic float mixdown. What it is NOT, deliberately: no occlusion, no HRTF, no
// doppler, no streaming, no voice handles you can stop or re-aim. Every one of those is a real
// feature and none of them is what "the block should make a noise when it falls over" needs.
//
// WHAT KEEPS A COLLAPSE FROM BUZZING (2026-10, measured on the city collapse): the original mixer
// started every play() it was handed. One charge produced 362 plays in a single tick, all the same
// short buffer starting on the same sample -- 48 coherent copies of one waveform, which sum into
// one loud pitch -- and then a play every tick or two, each restarting that buffer on the 60 Hz
// tick grid. Both are periodic, and periodic is what the ear calls a tone. So now: a sound class
// can forbid restarting the same buffer inside a window, plays pick among several recordings and
// jitter their speed a few percent, the voice cap evicts the quietest voice (by its real level,
// not the zero a not-yet-rendered voice happens to hold), and an optional limiter keeps a dense
// pile-up under full scale without the hard-clip crackle.
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

    // Per-GROUP voice budgets (index = SoundClass::group; 0 = no extra limit beyond max_voices).
    // Group 1 is destruction: a collapsing building may use at most this many of the voices, so
    // gunfire (group 0) always has the rest. Without a budget, a hundred falling bricks starve the
    // one sound the player is actually causing.
    std::array<std::uint32_t, 4> group_max_voices{0u, 20u, 0u, 0u};

    // Seed of the mixer's own PRNG, which picks variants and pitch jitter. Deterministic by
    // contract: the same play() sequence with the same seed renders the same samples.
    std::uint32_t seed = 0x5EED'A0D1u;

    // The shared reverb (see reverb.hpp). Sound classes choose how much of each voice enters it
    // via `SoundClass::reverb_send`; a class with no send costs nothing and a mixer that never
    // sees a send never runs the reverb at all.
    float reverb_decay_seconds = 1.5f;
    float reverb_damping = 0.35f;
    float reverb_wet = 1.0f; // master level of the wet signal

    // A peak limiter on the final stereo sum. Off by default so the clip counter keeps meaning
    // "the mix is too hot" in the tests that exist to catch that; a game with unbounded event
    // density turns it on. Mechanism: a short LOOKAHEAD ramps the gain down before a peak rather
    // than clamping it after the fact, because a clamp flat-tops the waveform and flat-topping is
    // the crackle you hear on a clipped explosion. The gain then releases slowly.
    bool limiter = false;
    float limiter_ceiling = 0.95f;
    float limiter_release_seconds = 0.20f;
};

// What a render did. As everywhere else in this engine, each counter exists because the failure it
// names is otherwise silent — and audio is the worst medium for silent failure, because nobody can
// see it and CI cannot hear it.
struct MixStats {
    std::uint64_t frames_rendered = 0;
    std::size_t voices_active = 0; // still sounding at the end of the block
    std::uint64_t voices_started = 0;
    std::uint64_t voices_stolen = 0;     // evicted by the cap to make room
    std::uint64_t voices_unknown = 0;    // play() named a sound the bank does not have
    std::uint64_t voices_suppressed = 0; // refused by the class retrigger window (the buzz guard)
    std::uint64_t voices_dropped = 0;    // refused because the cap was full of louder voices
    std::uint64_t voices_inaudible = 0;  // refused because the level was below audibility
    std::uint64_t limited_samples = 0;   // frames where the limiter reduced the gain
    std::uint64_t clipped_samples = 0;   // hit the [-1, 1] rail — the mix is too hot
    float peak = 0.0f;                   // largest absolute sample before clamping
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

    // Voices that count against the cap: those not already being faded out by an eviction. (An
    // evicted voice lingers for one block so it can glide to silence instead of clicking.)
    [[nodiscard]] std::size_t voice_count() const noexcept {
        std::size_t n = 0;
        for (const Voice& v : voices_) {
            n += v.fading ? 0u : 1u;
        }
        return n;
    }

private:
    struct Voice {
        SoundId sound = 0;
        std::size_t variant = 0;
        // Playback position in SAMPLES, fractional so a pitch-jittered voice (rate != 1) can read
        // between samples. A voice at rate exactly 1 only ever holds whole numbers here.
        double cursor = 0.0;
        float rate = 1.0f;
        float send = 0.0f;  // reverb send of this voice's class
        float level = 0.0f; // attenuation * gain * master: what the voice-cap ranks on
        float att = 0.0f;   // level before the per-voice gain, ramped for the reverb bus
        core::Vec3 position{0.0f, 0.0f, 0.0f};
        float gain = 1.0f;
        // Per-channel gains are held across blocks and RAMPED to their new targets inside each
        // block. Recomputing them per block and applying them as a step would click audibly every
        // time the listener turned — the classic zipper artefact — and interpolating is the whole
        // fix.
        float left = 0.0f;
        float right = 0.0f;
        std::uint8_t group = 0;
        bool fading = false;  // evicted: gliding to silence, removed after this block
        bool started = false; // false until the first block, so gains start AT target, not from 0
    };

    [[nodiscard]] float attenuation(float distance) const noexcept;
    // Per-channel gains for `v`, and the scalar level they share (before panning).
    void channel_gains(const Voice& v, float& left, float& right, float& level) const noexcept;
    [[nodiscard]] std::uint32_t next_random() noexcept;
    void apply_limiter(std::span<float> out, std::size_t frames);
    void evict(Voice& victim);

    SoundBank bank_;
    MixerConfig config_;
    Listener listener_;
    std::vector<Voice> voices_;
    MixStats stats_;

    std::uint32_t rng_ = 1;
    // The mixer's own clock in frames. Separate from `stats_.frames_rendered`, which
    // reset_stats() zeroes: retrigger windows must not forget what time it is.
    std::int64_t clock_ = 0;

    // Shuffle bag per sound id: every variant once, in random order, before any repeats.
    struct Bag {
        std::vector<std::uint16_t> order;
        std::size_t next = 0;
    };

    std::vector<Bag> bags_;
    std::vector<float> pending_power_;                  // per sound id: energy of refused plays
    std::vector<std::vector<std::int64_t>> last_start_; // per sound id, per variant: clock at start
    Reverb reverb_;
    std::vector<float> bus_, wet_left_, wet_right_;
    std::int64_t tail_frames_left_ = 0;
    float limiter_gain_ = 1.0f;
    std::vector<float> limit_curve_;
};

} // namespace rime::audio
