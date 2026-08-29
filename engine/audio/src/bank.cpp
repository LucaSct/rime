// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/audio/bank.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rime::audio {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// A seeded integer hash used as the noise source, NOT a std:: generator.
//
// The distinction is the whole reason the offline mixdown can assert exact samples: `std::mt19937`
// would be reproducible too, but `std::uniform_real_distribution` is explicitly NOT specified to
// produce the same values across implementations, so a libstdc++ run and an MSVC run would disagree
// and the proof would have to weaken to statistics. A hash of the sample index has no state at all
// — sample n is the same value however you got there, which also means a future streaming voice can
// resume mid-buffer without carrying a generator along.
[[nodiscard]] float noise(std::uint32_t n, std::uint32_t seed) noexcept {
    std::uint32_t h = n * 0x9E37'79B9u + seed * 0x85EB'CA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB'352Du;
    h ^= h >> 15;
    h *= 0x846C'A68Bu;
    h ^= h >> 16;
    // [-1, 1). 24 bits of mantissa is all a float carries anyway.
    return static_cast<float>(h & 0xFF'FFFFu) / 8388608.0f - 1.0f;
}

[[nodiscard]] std::size_t frames_for(float seconds) noexcept {
    return static_cast<std::size_t>(std::max(0.0f, seconds) * static_cast<float>(kSampleRate));
}

} // namespace

Sound noise_burst(float seconds, float cutoff01, float decay_rate, std::uint32_t seed) {
    Sound s;
    const std::size_t n = frames_for(seconds);
    s.samples.resize(n);

    // A ONE-POLE low-pass: y[i] = y[i-1] + a * (x[i] - y[i-1]). The cheapest filter there is, and
    // the right one here — impact noise wants its high end rolled off smoothly, not surgically, and
    // `a` maps directly onto "how bright is this material" with no design step in between.
    const float a = std::clamp(cutoff01, 0.001f, 1.0f);
    float y = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float x = noise(static_cast<std::uint32_t>(i), seed);
        y += a * (x - y);
        // Exponential decay. Amplitude envelopes are exponential rather than linear because
        // loudness is perceived logarithmically — a linear fade sounds like it hangs and then
        // stops, while an exponential one sounds like something ending.
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        s.samples[i] = y * std::exp(-decay_rate * t);
    }
    return s;
}

Sound decaying_sine(float seconds, float hz, float decay_rate) {
    Sound s;
    const std::size_t n = frames_for(seconds);
    s.samples.resize(n);
    const float w = 2.0f * kPi * hz / static_cast<float>(kSampleRate);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        s.samples[i] = std::sin(w * static_cast<float>(i)) * std::exp(-decay_rate * t);
    }
    return s;
}

void mix_into(Sound& a, const Sound& b, float gain) {
    if (b.samples.size() > a.samples.size()) {
        a.samples.resize(b.samples.size(), 0.0f);
    }
    for (std::size_t i = 0; i < b.samples.size(); ++i) {
        a.samples[i] += b.samples[i] * gain;
    }
}

SoundBank SoundBank::engine_defaults() {
    SoundBank bank;

    // Each of these is CRACK + BODY, the standard two-component impact recipe: a filtered noise
    // transient that says what the material is, and a decaying tone that says how big it is. The
    // seeds differ so two sounds playing together do not phase-cancel into one another, which is
    // what happens when the "random" part of several impacts is literally the same waveform.
    {
        Sound s = noise_burst(0.18f, 0.55f, 34.0f, 0x51ED'0001u);
        mix_into(s, decaying_sine(0.18f, 190.0f, 26.0f), 0.5f);
        bank.set(sound::kImpactConcrete, std::move(s));
    }
    {
        // A break is longer and lower than a survivable hit — more body, more time.
        Sound s = noise_burst(0.42f, 0.42f, 14.0f, 0x51ED'0002u);
        mix_into(s, decaying_sine(0.42f, 110.0f, 9.0f), 0.75f);
        bank.set(sound::kPartBreak, std::move(s));
    }
    {
        // Rubble coming to rest: brief, bright, quiet — mostly transient, almost no body.
        Sound s = noise_burst(0.13f, 0.80f, 46.0f, 0x51ED'0003u);
        mix_into(s, decaying_sine(0.13f, 320.0f, 40.0f), 0.22f);
        bank.set(sound::kDebrisSettle, std::move(s));
    }
    {
        // A collapse is the low one, and the long one. Two tones a fifth apart under a dull roar
        // reads as mass rather than as a single note.
        Sound s = noise_burst(1.30f, 0.14f, 3.2f, 0x51ED'0004u);
        mix_into(s, decaying_sine(1.30f, 55.0f, 2.4f), 0.9f);
        mix_into(s, decaying_sine(1.30f, 82.0f, 3.0f), 0.45f);
        bank.set(sound::kCollapse, std::move(s));
    }
    {
        Sound s = noise_burst(0.09f, 0.30f, 55.0f, 0x51ED'0005u);
        mix_into(s, decaying_sine(0.09f, 150.0f, 48.0f), 0.35f);
        bank.set(sound::kFootstep, std::move(s));
    }

    // Normalize each to a known peak so a gain of 1.0 means the same loudness whatever the recipe.
    // Without this, tuning a call site's gain would be tuning against whatever the synthesis
    // happened to produce, and every later change to a recipe would silently re-balance the mix.
    for (Sound& s : bank.sounds_) {
        float peak = 0.0f;
        for (const float v : s.samples) {
            peak = std::max(peak, std::fabs(v));
        }
        if (peak > 1e-6f) {
            const float inv = 0.9f / peak;
            for (float& v : s.samples) {
                v *= inv;
            }
        }
    }
    return bank;
}

void SoundBank::set(SoundId id, Sound sound) {
    if (id >= sounds_.size()) {
        sounds_.resize(static_cast<std::size_t>(id) + 1u);
    }
    sounds_[id] = std::move(sound);
}

std::span<const float> SoundBank::samples(SoundId id) const noexcept {
    if (id >= sounds_.size()) {
        return {};
    }
    return sounds_[id].samples;
}

} // namespace rime::audio
