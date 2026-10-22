// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.4 — the mixer, proven by OFFLINE MIXDOWN. No device, no sound card, no skipping.
//
// ADR-0035 asked for audio v1 "proven GPU-free by a deterministic offline mixdown", and the reason
// is blunt: **CI is deaf.** No runner has a playback device, so a proof that opened one would be a
// proof that skipped, on every machine that matters. Rendering into a buffer and asserting on the
// samples is available everywhere and is a strictly stronger claim than "it made a sound".
//
// The cases below are the properties a spatial mixer either has or does not, each written so that
// the failure it catches is a thing you would otherwise notice only by ear — which, headless, means
// never.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "rime/audio/bank.hpp"
#include "rime/audio/mixer.hpp"

using namespace rime;
using namespace rime::audio;

namespace {

constexpr std::size_t kBlock = 512; // frames
constexpr float kPi = std::numbers::pi_v<float>;

[[nodiscard]] std::vector<float> block() {
    return std::vector<float>(kBlock * 2u, 0.0f);
}

// Per-channel RMS of an interleaved stereo buffer. RMS rather than peak because it is what
// "loudness" means over a block and it does not hinge on catching one sample.
struct Rms {
    double left = 0.0;
    double right = 0.0;
};

[[nodiscard]] Rms rms(const std::vector<float>& buf) {
    double l = 0.0;
    double r = 0.0;
    const std::size_t frames = buf.size() / 2u;
    for (std::size_t i = 0; i < frames; ++i) {
        l += static_cast<double>(buf[i * 2u]) * buf[i * 2u];
        r += static_cast<double>(buf[i * 2u + 1u]) * buf[i * 2u + 1u];
    }
    return {std::sqrt(l / static_cast<double>(frames)), std::sqrt(r / static_cast<double>(frames))};
}

// A listener at the origin facing −Z (identity orientation), so +X is its right.
[[nodiscard]] Listener origin_listener() {
    return Listener{};
}

} // namespace

TEST_CASE("m13.4: the mixdown is deterministic, which is the whole proof surface") {
    const auto play_and_render = []() {
        Mixer mixer(SoundBank::engine_defaults());
        mixer.set_listener(origin_listener());
        std::vector<float> all;
        for (int b = 0; b < 8; ++b) {
            if (b == 0) {
                mixer.play(sound::kPartBreak, {3.0f, 0.0f, -4.0f}, 1.0f);
            }
            if (b == 2) {
                mixer.play(sound::kDebrisSettle, {-6.0f, 1.0f, -2.0f}, 0.7f);
                mixer.play(sound::kImpactConcrete, {0.0f, 0.0f, -5.0f}, 0.9f);
            }
            std::vector<float> buf = block();
            mixer.render(buf);
            all.insert(all.end(), buf.begin(), buf.end());
        }
        return all;
    };

    const std::vector<float> a = play_and_render();
    const std::vector<float> b = play_and_render();

    // BIT-identical, not approximately equal. The synthesis uses a hash of the sample index rather
    // than a std:: distribution precisely so this can be an exact claim across implementations.
    CHECK(a == b);

    // Non-vacuity: it really did make sound. Two all-zero buffers are also bit-identical.
    const Rms level = rms(a);
    CHECK(level.left > 1e-4);
    CHECK(level.right > 1e-4);
}

TEST_CASE("m13.4: silence is silent, and a voice retires when it ends") {
    Mixer mixer(SoundBank::engine_defaults());
    mixer.set_listener(origin_listener());

    std::vector<float> quiet = block();
    mixer.render(quiet);
    CHECK(std::all_of(quiet.begin(), quiet.end(), [](float v) { return v == 0.0f; }));
    CHECK(mixer.voice_count() == 0);

    mixer.play(sound::kDebrisSettle, {0.0f, 0.0f, -3.0f}, 1.0f);
    CHECK(mixer.voice_count() == 1);

    std::vector<float> loud = block();
    mixer.render(loud);
    CHECK(rms(loud).left > 1e-4);

    // kDebrisSettle is 0.13 s ≈ 6240 frames; render well past it and the voice must be GONE, not
    // merely quiet. A mixer that never retires voices runs out of slots and then stops making the
    // sounds that matter — the audio twin of the m13.2b render-leaf leak.
    for (int i = 0; i < 40; ++i) {
        std::vector<float> buf = block();
        mixer.render(buf);
    }
    CHECK(mixer.voice_count() == 0);
    CHECK(mixer.stats().voices_active == 0);

    std::vector<float> after = block();
    mixer.render(after);
    CHECK(std::all_of(after.begin(), after.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("m13.4: panning puts a sound where it actually is") {
    const auto level_at = [](core::Vec3 pos) {
        Mixer mixer(SoundBank::engine_defaults());
        mixer.set_listener(origin_listener());
        mixer.play(sound::kPartBreak, pos, 1.0f);
        std::vector<float> buf = block();
        mixer.render(buf);
        return rms(buf);
    };

    // Listener faces −Z, so +X is its right.
    const Rms right = level_at({8.0f, 0.0f, 0.0f});
    CHECK(right.right > right.left * 3.0);

    const Rms left = level_at({-8.0f, 0.0f, 0.0f});
    CHECK(left.left > left.right * 3.0);

    // Dead ahead is centred — and the two channels must match closely, not merely be "similar".
    const Rms ahead = level_at({0.0f, 0.0f, -8.0f});
    CHECK(ahead.left == doctest::Approx(ahead.right).epsilon(0.02));

    SUBCASE("and does so at CONSTANT POWER, so a pass-by does not duck in the middle") {
        // The property that separates constant-power from linear panning. Linear panning sums to
        // constant AMPLITUDE, which is ~3 dB quieter at centre than at the edges — a source moving
        // past the listener audibly dips exactly as it goes by, which is the moment it should be
        // most present. Summing SQUARES is what stays flat.
        double lowest = 1e9;
        double highest = 0.0;
        for (int i = -4; i <= 4; ++i) {
            const float angle = static_cast<float>(i) * (kPi / 8.0f);
            const Rms r = level_at({std::sin(angle) * 8.0f, 0.0f, -std::cos(angle) * 8.0f});
            const double power = r.left * r.left + r.right * r.right;
            lowest = std::min(lowest, power);
            highest = std::max(highest, power);
        }
        REQUIRE(lowest > 0.0);
        // Within 12%: the residual is the source's varying DISTANCE around the arc and the block's
        // gain ramp, not the pan law.
        CHECK(highest / lowest < 1.12);
    }
}

TEST_CASE("m13.4: distance attenuates monotonically and reaches actual silence") {
    MixerConfig cfg;
    cfg.reference_distance = 2.0f;
    cfg.max_distance = 40.0f;

    const auto level_at_distance = [&cfg](float d) {
        Mixer mixer(SoundBank::engine_defaults(), cfg);
        mixer.set_listener(origin_listener());
        mixer.play(sound::kCollapse, {0.0f, 0.0f, -d}, 1.0f);
        std::vector<float> buf = block();
        mixer.render(buf);
        const Rms r = rms(buf);
        return r.left + r.right;
    };

    double previous = level_at_distance(1.0f);
    CHECK(previous > 0.0);
    for (const float d : {2.0f, 4.0f, 8.0f, 16.0f, 30.0f, 39.0f}) {
        const double here = level_at_distance(d);
        CHECK(here <= previous);
        previous = here;
    }

    // EXACTLY zero past max_distance, not merely small. A pure inverse-square law never reaches
    // silence, so distant voices would sound forever and hold voice slots a near sound wanted —
    // which is why the model has a linear taper on top of it.
    CHECK(level_at_distance(40.0f) == doctest::Approx(0.0));
    CHECK(level_at_distance(200.0f) == doctest::Approx(0.0));
}

TEST_CASE("m13.4: the voice cap steals rather than drops, and says so") {
    MixerConfig cfg;
    cfg.max_voices = 4;
    Mixer mixer(SoundBank::engine_defaults(), cfg);
    mixer.set_listener(origin_listener());

    // Four distant (quiet) voices, then a close loud one. The close one MUST be heard: refusing a
    // new voice when the cap is full makes the biggest moments the ones most likely to go silent,
    // which is exactly backwards.
    for (int i = 0; i < 4; ++i) {
        mixer.play(sound::kCollapse, {0.0f, 0.0f, -35.0f - static_cast<float>(i)}, 1.0f);
    }
    std::vector<float> warm = block();
    mixer.render(warm); // establishes each voice's current level, which is what stealing ranks on
    CHECK(mixer.voice_count() == 4);

    mixer.play(sound::kPartBreak, {0.0f, 0.0f, -1.0f}, 1.0f);
    CHECK(mixer.voice_count() == 4); // capped
    CHECK(mixer.stats().voices_stolen == 1);

    std::vector<float> buf = block();
    mixer.render(buf);
    // The close sound dominates: far louder than the four distant ones managed together.
    CHECK(rms(buf).left > rms(warm).left * 2.0);
}

TEST_CASE("m13.4: an unknown sound is a COUNT, not a wrong noise") {
    Mixer mixer(SoundBank::engine_defaults());
    mixer.set_listener(origin_listener());

    mixer.play(9999, {0.0f, 0.0f, -1.0f}, 1.0f);
    CHECK(mixer.stats().voices_unknown == 1);
    CHECK(mixer.stats().voices_started == 0);
    CHECK(mixer.voice_count() == 0);

    std::vector<float> buf = block();
    mixer.render(buf);
    CHECK(std::all_of(buf.begin(), buf.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("m13.4: clipping is counted, because nobody can see it and CI cannot hear it") {
    MixerConfig cfg;
    cfg.master_gain = 1.0f;
    Mixer mixer(SoundBank::engine_defaults(), cfg);
    mixer.set_listener(origin_listener());

    SUBCASE("a sane mix does not clip") {
        mixer.play(sound::kImpactConcrete, {0.0f, 0.0f, -3.0f}, 0.8f);
        for (int i = 0; i < 20; ++i) {
            std::vector<float> buf = block();
            mixer.render(buf);
        }
        CHECK(mixer.stats().clipped_samples == 0);
        CHECK(mixer.stats().peak > 0.0f);
        CHECK(mixer.stats().peak <= 1.0f);
    }

    SUBCASE("a deliberately hot one does, and reports it") {
        // Sixteen coincident full-gain voices at the reference distance. This is the failure the
        // counter exists for: it is not a crash, not a warning, and not visible — just distortion,
        // on a machine where nobody is listening.
        for (int i = 0; i < 16; ++i) {
            mixer.play(sound::kPartBreak, {0.0f, 0.0f, -1.0f}, 1.0f);
        }
        std::vector<float> buf = block();
        mixer.render(buf);
        CHECK(mixer.stats().clipped_samples > 0);
        // …and the output is still in range: counted AND clamped, never wrapped.
        CHECK(std::all_of(buf.begin(), buf.end(), [](float v) { return v >= -1.0f && v <= 1.0f; }));
    }
}

TEST_CASE("m13.4: the bank is deterministic and every engine sound is present") {
    const SoundBank a = SoundBank::engine_defaults();
    const SoundBank b = SoundBank::engine_defaults();

    for (const SoundId id : {sound::kImpactConcrete,
                             sound::kPartBreak,
                             sound::kDebrisSettle,
                             sound::kCollapse,
                             sound::kFootstep}) {
        const std::span<const float> sa = a.samples(id);
        const std::span<const float> sb = b.samples(id);
        REQUIRE_FALSE(sa.empty());
        REQUIRE(sa.size() == sb.size());
        CHECK(std::equal(sa.begin(), sa.end(), sb.begin()));

        // Normalized to a known peak, so a call site's gain means the same loudness whatever the
        // synthesis recipe. Without this, tuning a gain would be tuning against whatever the
        // generator happened to produce.
        float peak = 0.0f;
        for (const float v : sa) {
            peak = std::max(peak, std::fabs(v));
        }
        CHECK(peak == doctest::Approx(0.9f).epsilon(0.01));
    }

    // A collapse is the long one and a footstep the short one — if those ever invert, someone has
    // swapped the recipes.
    CHECK(a.samples(sound::kCollapse).size() > a.samples(sound::kFootstep).size() * 8);
}

// ── Early reflections (ADR-0035 / M13 geometric acoustics) ──────────────────────────────────
// Discrete echoes modeled via the image-source method:
// (a) sample-accurate onset timing, including mid-block;
// (b) 4 kHz one-pole low-pass filtering attenuating highs;
// (c) delay clamping to [0.030, 0.200] s and dropping extra reflections past 6;
// (d) sensitivity control: bit-identical output to play() when reflections are empty.

TEST_CASE("m13.4: reflections (a) onset arrives sample-accurately, even mid-block") {
    SoundBank bank;
    constexpr SoundId kImpulse = 200;
    Sound imp;
    imp.samples.assign(64, 0.0f);
    imp.samples[0] = 1.0f; // single-sample impulse
    bank.set(kImpulse, std::move(imp));
    bank.set_class(kImpulse, SoundClass{.pitch_jitter = 0.0f, .gain_jitter_db = 0.0f});

    Mixer mixer(std::move(bank));
    mixer.set_listener(origin_listener());

    // 0.050 s at 48 kHz is 2400 frames.
    // With 512-frame blocks: 2400 = 4 * 512 + 352, which lands 352 frames into block 4 (mid-block).
    const EarlyReflection refl{
        .position = {0.0f, 0.0f, -1.0f},
        .delay_seconds = 0.050f,
        .gain = 1.0f,
    };
    mixer.play_with_reflections(kImpulse, {0.0f, 0.0f, -1.0f}, 1.0f, {&refl, 1});

    std::vector<float> rendered;
    for (int b = 0; b < 10; ++b) {
        std::vector<float> buf = block();
        mixer.render(buf);
        rendered.insert(rendered.end(), buf.begin(), buf.end());
    }

    // Direct sound onset is at frame 0.
    REQUIRE(std::fabs(rendered[0]) > 1e-4f);

    // Search for the first non-zero frame of the reflection (after the direct impulse ends).
    std::size_t refl_onset = 0;
    for (std::size_t i = 100; i < rendered.size() / 2u; ++i) {
        if (std::fabs(rendered[i * 2u]) > 1e-4f) {
            refl_onset = i;
            break;
        }
    }

    REQUIRE(refl_onset > 0);
    // Reflection onset must be within +-1 frame of 0.050 * sample_rate (2400).
    const auto diff = std::abs(static_cast<std::int64_t>(refl_onset) - 2400);
    CHECK(diff <= 1);
    // And verify that it landed mid-block (not at a block boundary).
    CHECK(refl_onset % kBlock != 0);
}

TEST_CASE("m13.4: reflections (b) low-pass filter (~4 kHz) attenuates high-band energy") {
    // Generate white noise to test the frequency response across the audible spectrum.
    SoundBank bank;
    constexpr SoundId kNoise = 201;
    Sound noise;
    noise.samples.resize(2400);
    std::uint32_t rng = 0xACE1'4829u;
    for (float& s : noise.samples) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        s = (static_cast<float>(rng & 0xFFFFu) / 32768.0f) - 1.0f;
    }
    bank.set(kNoise, std::move(noise));
    bank.set_class(kNoise, SoundClass{.pitch_jitter = 0.0f, .gain_jitter_db = 0.0f});

    // Simple high-pass difference filter y[n] = x[n] - x[n-1] to measure high-band energy (>8 kHz).
    const auto high_band_power = [](std::span<const float> interleaved_stereo,
                                    std::size_t start_frame,
                                    std::size_t num_frames) {
        double power = 0.0;
        for (std::size_t i = start_frame + 1; i < start_frame + num_frames; ++i) {
            const double s_curr = interleaved_stereo[i * 2u];
            const double s_prev = interleaved_stereo[(i - 1) * 2u];
            const double diff = s_curr - s_prev;
            power += diff * diff;
        }
        return power / static_cast<double>(num_frames - 1);
    };

    // 1. Direct sound alone:
    Mixer direct_mixer(bank);
    direct_mixer.set_listener(origin_listener());
    direct_mixer.play(kNoise, {0.0f, 0.0f, -2.0f}, 1.0f);
    std::vector<float> direct_out;
    for (int b = 0; b < 10; ++b) {
        std::vector<float> buf = block();
        direct_mixer.render(buf);
        direct_out.insert(direct_out.end(), buf.begin(), buf.end());
    }
    const double direct_high_energy = high_band_power(direct_out, 0, 2400);
    REQUIRE(direct_high_energy > 1e-4);

    // 2. Sound with reflection at 0.060 s (2880 frames delay, non-overlapping with direct sound):
    Mixer refl_mixer(bank);
    refl_mixer.set_listener(origin_listener());
    const EarlyReflection refl{
        .position = {0.0f, 0.0f, -2.0f},
        .delay_seconds = 0.060f,
        .gain = 1.0f,
    };
    refl_mixer.play_with_reflections(kNoise, {0.0f, 0.0f, -2.0f}, 1.0f, {&refl, 1});
    std::vector<float> refl_out;
    for (int b = 0; b < 15; ++b) {
        std::vector<float> buf = block();
        refl_mixer.render(buf);
        refl_out.insert(refl_out.end(), buf.begin(), buf.end());
    }
    // Reflection is active from frame 2880 to 2880 + 2400 = 5280.
    const double refl_high_energy = high_band_power(refl_out, 2880, 2400);
    REQUIRE(refl_high_energy > 1e-4);

    // The reflection's high-band energy must be lower than the direct sound's by a clear margin
    // (a ~4 kHz low-pass rolls off highs by >6 dB per octave).
    CHECK(refl_high_energy < direct_high_energy * 0.40);
}

TEST_CASE("m13.4: reflections (c) delay clamping and reflection dropping past cap") {
    SoundBank bank;
    constexpr SoundId kImp = 202;
    Sound imp;
    imp.samples.assign(32, 0.0f);
    imp.samples[0] = 1.0f;
    bank.set(kImp, std::move(imp));
    bank.set_class(kImp, SoundClass{.pitch_jitter = 0.0f, .gain_jitter_db = 0.0f});

    SUBCASE("delays outside [0.030, 0.200] are clamped") {
        Mixer mixer(bank);
        mixer.set_listener(origin_listener());

        // Under 30 ms (e.g. 10 ms = 480 frames): clamped to 30 ms = 1440 frames.
        const EarlyReflection too_short{
            .position = {0.0f, 0.0f, -1.0f},
            .delay_seconds = 0.010f,
            .gain = 1.0f,
        };
        mixer.play_with_reflections(kImp, {0.0f, 0.0f, -1.0f}, 1.0f, {&too_short, 1});

        std::vector<float> rendered;
        for (int b = 0; b < 10; ++b) {
            std::vector<float> buf = block();
            mixer.render(buf);
            rendered.insert(rendered.end(), buf.begin(), buf.end());
        }

        // Must NOT appear at 480 frames:
        for (std::size_t i = 100; i < 1400; ++i) {
            CHECK(rendered[i * 2u] == 0.0f);
        }
        // Must appear at 1440 frames (+-1 frame):
        std::size_t onset = 0;
        for (std::size_t i = 100; i < rendered.size() / 2u; ++i) {
            if (std::fabs(rendered[i * 2u]) > 1e-4f) {
                onset = i;
                break;
            }
        }
        CHECK(std::abs(static_cast<std::int64_t>(onset) - 1440) <= 1);
    }

    SUBCASE("a 7th reflection increments reflections_dropped") {
        Mixer mixer(bank);
        mixer.set_listener(origin_listener());

        std::vector<EarlyReflection> seven(7);
        for (std::size_t i = 0; i < 7; ++i) {
            seven[i] = EarlyReflection{
                .position = {0.0f, 0.0f, -2.0f},
                .delay_seconds = 0.030f + static_cast<float>(i) * 0.010f,
                .gain = 0.5f,
            };
        }

        mixer.play_with_reflections(kImp, {0.0f, 0.0f, -1.0f}, 1.0f, seven);
        CHECK(mixer.stats().reflections_dropped == 1);

        // 8 reflections drops 2:
        seven.push_back(EarlyReflection{
            .position = {0.0f, 0.0f, -2.0f},
            .delay_seconds = 0.120f,
            .gain = 0.5f,
        });
        mixer.play_with_reflections(kImp, {0.0f, 0.0f, -1.0f}, 1.0f, seven);
        CHECK(mixer.stats().reflections_dropped == 3);
    }
}

TEST_CASE(
    "m13.4: reflections (d) sensitivity control: zero reflections is bit-identical to play()") {
    MixerConfig cfg;
    cfg.seed = 0x9876'5432u;
    Mixer m1(SoundBank::engine_defaults(), cfg);
    Mixer m2(SoundBank::engine_defaults(), cfg);
    m1.set_listener(origin_listener());
    m2.set_listener(origin_listener());

    std::vector<float> all1, all2;
    for (int b = 0; b < 16; ++b) {
        if (b == 0) {
            m1.play(sound::kPartBreak, {3.0f, 0.0f, -4.0f}, 0.9f);
            m2.play_with_reflections(sound::kPartBreak, {3.0f, 0.0f, -4.0f}, 0.9f, {});
        }
        if (b == 3) {
            m1.play(sound::kImpactConcrete, {-2.0f, 1.0f, -3.0f}, 0.8f);
            m2.play_with_reflections(sound::kImpactConcrete, {-2.0f, 1.0f, -3.0f}, 0.8f, {});
        }
        if (b == 7) {
            m1.play(sound::kDebrisSettle, {0.0f, 0.0f, -5.0f}, 0.7f);
            m2.play_with_reflections(sound::kDebrisSettle, {0.0f, 0.0f, -5.0f}, 0.7f, {});
        }
        std::vector<float> buf1 = block();
        std::vector<float> buf2 = block();
        m1.render(buf1);
        m2.render(buf2);
        all1.insert(all1.end(), buf1.begin(), buf1.end());
        all2.insert(all2.end(), buf2.begin(), buf2.end());
    }

    CHECK(all1 == all2);
    // Non-vacuity: sound was actually produced.
    CHECK(rms(all1).left > 1e-4);
    CHECK(m2.stats().reflections_dropped == 0);
}
