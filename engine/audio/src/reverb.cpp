// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/audio/reverb.hpp"

#include <algorithm>
#include <cmath>

namespace rime::audio {
namespace {

// Comb delays in samples at 48 kHz (33-49 ms). Chosen by hand to be mutually non-harmonic: if two
// delays shared a common factor their echoes would coincide and the tail would ring at that
// period. The right channel adds a small offset (the stereo spread).
constexpr std::size_t kCombDelays[4] = {1559, 1831, 2087, 2339};
constexpr std::size_t kAllPassDelays[2] = {347, 113};
constexpr std::size_t kStereoSpread = 29;
constexpr float kAllPassGain = 0.5f;
// Output trim: four parallel combs sum to a hot signal; this keeps send = 1 roughly unity.
constexpr float kWetTrim = 0.30f;

} // namespace

Reverb::Reverb(const ReverbConfig& config)
    : config_(config), damp_(std::clamp(config.damping, 0.0f, 0.98f)) {
    const float scale = static_cast<float>(config.sample_rate) / 48000.0f;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < kCombs; ++i) {
            const auto delay = static_cast<std::size_t>(
                static_cast<float>(kCombDelays[i] + ch * kStereoSpread) * scale);
            Comb& c = combs_[ch][i];
            c.line.assign(std::max<std::size_t>(delay, 2), 0.0f);
            // RT60 relation: after one trip (delay seconds) the loop must have lost
            // 60 * delay / RT60 dB, i.e. a gain of 10^(-3 * delay / RT60).
            const float delay_s =
                static_cast<float>(c.line.size()) / static_cast<float>(config.sample_rate);
            c.feedback = std::pow(10.0f, -3.0f * delay_s / std::max(0.1f, config.decay_seconds));
        }
        for (std::size_t i = 0; i < kAllPasses; ++i) {
            const auto delay = static_cast<std::size_t>(
                static_cast<float>(kAllPassDelays[i] + ch * kStereoSpread / 2) * scale);
            allpasses_[ch][i].line.assign(std::max<std::size_t>(delay, 2), 0.0f);
        }
    }
}

void Reverb::clear() noexcept {
    for (auto& ch : combs_) {
        for (Comb& c : ch) {
            std::fill(c.line.begin(), c.line.end(), 0.0f);
            c.damp_state = 0.0f;
            c.cursor = 0;
        }
    }
    for (auto& ch : allpasses_) {
        for (AllPass& a : ch) {
            std::fill(a.line.begin(), a.line.end(), 0.0f);
            a.cursor = 0;
        }
    }
    hp_in_ = hp_out_ = 0.0f;
}

void Reverb::process(std::span<const float> in, std::span<float> left, std::span<float> right) {
    const std::size_t n = std::min({in.size(), left.size(), right.size()});
    // One-pole high-pass at ~120 Hz: y[i] = a * (y[i-1] + x[i] - x[i-1]).
    const float hp_a =
        std::exp(-2.0f * 3.14159265f * 120.0f / static_cast<float>(config_.sample_rate));
    for (std::size_t i = 0; i < n; ++i) {
        const float x = in[i];
        hp_out_ = hp_a * (hp_out_ + x - hp_in_);
        hp_in_ = x;
        const float dry = hp_out_;

        float wet[2] = {0.0f, 0.0f};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            float acc = 0.0f;
            for (Comb& c : combs_[ch]) {
                const float delayed = c.line[c.cursor];
                // The damping filter sits INSIDE the feedback path: the signal that comes round
                // again is a low-passed copy, so each repeat is duller than the last.
                c.damp_state = delayed * (1.0f - damp_) + c.damp_state * damp_;
                c.line[c.cursor] = dry + c.damp_state * c.feedback;
                c.cursor = (c.cursor + 1) % c.line.size();
                acc += delayed;
            }
            // Schroeder all-pass: y = -g*x + buf; buf = x + g*y. Flat magnitude, scrambled phase.
            for (AllPass& a : allpasses_[ch]) {
                const float buf = a.line[a.cursor];
                const float y = -kAllPassGain * acc + buf;
                a.line[a.cursor] = acc + kAllPassGain * y;
                a.cursor = (a.cursor + 1) % a.line.size();
                acc = y;
            }
            wet[ch] = acc * kWetTrim;
        }
        left[i] += wet[0];
        right[i] += wet[1];
    }
}

} // namespace rime::audio
