// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/audio/bank.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "rime/audio/wav.hpp"

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

namespace {

// One synthetic variant: a bright noise CRACK over a dull noise THUD.
//
// The body used to be a decaying sine. A sine has exactly one frequency, so every copy of it that
// the mixer stacked reinforced the same line in the spectrum -- measured on the city collapse, 42 %
// of the whole mix's energy sat in one 109 Hz bin, which is a hum, not rubble. A heavily low-passed
// NOISE burst has the same "low and heavy" character but spreads its energy over a band, and two
// copies with different seeds are uncorrelated, so they add as power (+3 dB), never as a coherent
// pile-up (+6 dB per doubling and a pitch).
[[nodiscard]] Sound crack_and_thud(float seconds,
                                   float crack_cutoff,
                                   float crack_decay,
                                   float thud_cutoff,
                                   float thud_decay,
                                   float thud_gain,
                                   std::uint32_t seed) {
    Sound s = noise_burst(seconds, crack_cutoff, crack_decay, seed);
    // A one-pole at a tiny coefficient is very quiet; the bank normalises afterwards, so only the
    // balance between the layers matters. The gain is scaled up to compensate for that loss.
    mix_into(
        s, noise_burst(seconds, thud_cutoff, thud_decay, seed ^ 0xA5A5'0000u), thud_gain * 9.0f);
    return s;
}

// A long, slow, low swell: the bed under a collapse. Noise through a very low one-pole (~75 Hz),
// with an attack so it rises rather than starts, then an exponential release.
[[nodiscard]] Sound rumble_bed(float seconds, float cutoff01, float release, std::uint32_t seed) {
    Sound s = noise_burst(seconds, cutoff01, 0.0f, seed);
    for (std::size_t i = 0; i < s.samples.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        const float attack = std::min(1.0f, t / 0.25f);
        s.samples[i] *= attack * std::exp(-release * t);
    }
    return s;
}

} // namespace

SoundBank SoundBank::engine_defaults() {
    SoundBank bank;

    // Each variant is CRACK + THUD, the standard two-component impact recipe: a filtered noise
    // transient that says what the material is, and a darker noise body that says how big it is.
    // The seeds differ so two sounds playing together do not phase-cancel into one another, which
    // is what happens when the "random" part of several impacts is literally the same waveform;
    // the parameters differ a little so the variants are not four copies of one hit.
    for (std::uint32_t v = 0; v < 4; ++v) {
        const float k = static_cast<float>(v);
        bank.add_variant(sound::kImpactConcrete,
                         crack_and_thud(0.18f,
                                        0.55f - 0.04f * k,
                                        34.0f + 3.0f * k,
                                        0.035f,
                                        26.0f,
                                        0.5f,
                                        0x51ED'0001u + v * 0x101u));
        // A break is longer and lower than a survivable hit -- more body, more time.
        bank.add_variant(sound::kPartBreak,
                         crack_and_thud(0.42f,
                                        0.42f - 0.03f * k,
                                        14.0f + 1.5f * k,
                                        0.016f,
                                        9.0f,
                                        0.75f,
                                        0x51ED'0002u + v * 0x101u));
        // Rubble coming to rest: brief, bright, quiet -- mostly transient, almost no body.
        bank.add_variant(sound::kDebrisSettle,
                         crack_and_thud(0.13f + 0.01f * k,
                                        0.80f - 0.08f * k,
                                        46.0f - 3.0f * k,
                                        0.05f,
                                        40.0f,
                                        0.22f,
                                        0x51ED'0003u + v * 0x101u));
        // A collapse is the low one, and the long one: a dull roar over two overlapping low noise
        // bands (not two tones -- see crack_and_thud) so it reads as mass without a pitch.
        Sound c = crack_and_thud(1.30f,
                                 0.14f - 0.01f * k,
                                 3.2f + 0.2f * k,
                                 0.010f,
                                 2.4f,
                                 0.9f,
                                 0x51ED'0004u + v * 0x101u);
        mix_into(c, noise_burst(1.30f, 0.017f, 3.0f, 0x51ED'0404u + v * 0x101u), 4.0f);
        bank.add_variant(sound::kCollapse, std::move(c));
        bank.add_variant(
            sound::kFootstep,
            crack_and_thud(0.09f, 0.30f, 55.0f, 0.04f, 48.0f, 0.35f, 0x51ED'0005u + v * 0x101u));
    }
    bank.add_variant(sound::kRumble, rumble_bed(2.0f, 0.009f, 1.1f, 0x51ED'0006u));
    bank.add_variant(sound::kRumble, rumble_bed(2.4f, 0.011f, 0.9f, 0x51ED'0106u));

    // Normalize each variant to a known peak so a gain of 1.0 means the same loudness whatever the
    // recipe. Without this, tuning a call site's gain would be tuning against whatever the
    // synthesis happened to produce, and every later change to a recipe would silently re-balance
    // the mix.
    for (Entry& e : bank.entries_) {
        for (Sound& s : e.variants) {
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
    }
    return bank;
}

void SoundBank::apply_destruction_policy() {
    // Impacts jitter in pitch and may not restart one recording inside 40 ms (the buzz guard); a
    // collapse is a rarer, bigger event, so its window is wider; the rumble bed is one per
    // episode. Rubble touches the reverb a little, a break more, a collapse a fair amount.
    set_class(sound::kImpactConcrete, {0.04f, 0.04f, 0.12f});
    set_class(sound::kPartBreak, {0.04f, 0.04f, 0.18f});
    set_class(sound::kDebrisSettle, {0.05f, 0.04f, 0.08f});
    set_class(sound::kCollapse, {0.03f, 0.25f, 0.30f});
    set_class(sound::kFootstep, {0.05f, 0.10f, 0.0f});
    set_class(sound::kRumble, {0.02f, 1.2f, 0.25f});
}

void SoundBank::set(SoundId id, Sound sound) {
    clear(id);
    add_variant(id, std::move(sound));
}

void SoundBank::add_variant(SoundId id, Sound sound) {
    if (id >= entries_.size()) {
        entries_.resize(static_cast<std::size_t>(id) + 1u);
    }
    entries_[id].variants.push_back(std::move(sound));
}

bool SoundBank::add_variant_from_wav(SoundId id, const std::filesystem::path& path) {
    auto wav = read_wav(path);
    if (!wav || wav->sample_rate != kSampleRate || wav->channels != 1 || wav->samples.empty()) {
        return false;
    }
    Sound s;
    s.samples = std::move(wav->samples);
    add_variant(id, std::move(s));
    return true;
}

void SoundBank::clear(SoundId id) {
    if (id < entries_.size()) {
        entries_[id].variants.clear();
    }
}

void SoundBank::set_class(SoundId id, const SoundClass& cls) {
    if (id >= entries_.size()) {
        entries_.resize(static_cast<std::size_t>(id) + 1u);
    }
    entries_[id].cls = cls;
}

const SoundClass& SoundBank::sound_class(SoundId id) const noexcept {
    static const SoundClass kDefault{};
    return id < entries_.size() ? entries_[id].cls : kDefault;
}

std::span<const float> SoundBank::samples(SoundId id) const noexcept {
    return samples(id, 0);
}

std::span<const float> SoundBank::samples(SoundId id, std::size_t variant) const noexcept {
    if (id >= entries_.size() || variant >= entries_[id].variants.size()) {
        return {};
    }
    return entries_[id].variants[variant].samples;
}

std::size_t SoundBank::variant_count(SoundId id) const noexcept {
    return id < entries_.size() ? entries_[id].variants.size() : 0u;
}

} // namespace rime::audio
