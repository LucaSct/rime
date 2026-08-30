// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rime::audio {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

} // namespace

Mixer::Mixer(SoundBank bank, const MixerConfig& config) : bank_(std::move(bank)), config_(config) {
    voices_.reserve(config_.max_voices);
}

float Mixer::attenuation(float distance) const noexcept {
    const float ref = std::max(0.001f, config_.reference_distance);
    if (distance <= ref) {
        return 1.0f;
    }
    if (distance >= config_.max_distance) {
        return 0.0f;
    }
    // Inverse distance for the shape the ear expects, times a linear taper so the level reaches
    // EXACTLY zero at max_distance instead of merely approaching it. Without the taper a distant
    // voice never ends, and every one of them holds a voice slot a near sound wanted.
    const float inverse = ref / distance;
    const float taper = (config_.max_distance - distance) / (config_.max_distance - ref);
    return inverse * taper;
}

void Mixer::channel_gains(const Voice& v, float& left, float& right) const noexcept {
    const core::Vec3 delta = v.position - listener_.position;
    const float distance = core::length(delta);
    const float att = attenuation(distance) * v.gain * config_.master_gain;

    // Pan by where the source sits along the listener's RIGHT axis. A source directly ahead, behind
    // or overhead all pan to centre — two-channel audio genuinely cannot distinguish front from
    // back, and pretending otherwise with a fake filter is worse than not claiming it.
    float pan = 0.0f;
    if (distance > 1e-4f) {
        const core::Vec3 right_axis = core::rotate(listener_.orientation, {1.0f, 0.0f, 0.0f});
        pan = std::clamp(core::dot(delta * (1.0f / distance), right_axis), -1.0f, 1.0f);
    }

    // CONSTANT POWER panning: map pan ∈ [−1, 1] onto an angle in [0, π/2] and take cos/sin. The sum
    // of SQUARES is then constant, so a source sweeping past the listener holds its apparent
    // loudness. Linear panning (0.5/0.5 at centre) sums to constant AMPLITUDE instead, which dips
    // ~3 dB in the middle and makes anything crossing the centre audibly duck.
    const float angle = (pan + 1.0f) * (kPi * 0.25f);
    left = std::cos(angle) * att;
    right = std::sin(angle) * att;
}

void Mixer::play(SoundId sound, core::Vec3 position, float gain) {
    if (bank_.samples(sound).empty()) {
        // An id the bank has nothing for is SILENCE and a counter, never a wrong sound. Without the
        // counter this is the most invisible failure in the engine: no crash, no log, and audio
        // that is simply missing for one event type.
        ++stats_.voices_unknown;
        return;
    }

    if (voices_.size() >= config_.max_voices) {
        // Steal the quietest. Deterministic on ties by taking the FIRST minimum, which is also the
        // oldest — so a wall of equally-quiet rubble sheds its longest-running voice, which is the
        // one closest to finishing anyway.
        auto quietest = voices_.begin();
        float lowest = quietest->left + quietest->right;
        for (auto it = voices_.begin(); it != voices_.end(); ++it) {
            const float level = it->left + it->right;
            if (level < lowest) {
                lowest = level;
                quietest = it;
            }
        }
        voices_.erase(quietest);
        ++stats_.voices_stolen;
    }

    Voice v;
    v.sound = sound;
    v.position = position;
    v.gain = gain;
    voices_.push_back(v);
    ++stats_.voices_started;
}

void Mixer::render(std::span<float> out) {
    std::fill(out.begin(), out.end(), 0.0f);
    const std::size_t frames = out.size() / 2u;
    if (frames == 0) {
        return;
    }

    for (Voice& v : voices_) {
        const std::span<const float> src = bank_.samples(v.sound);
        float target_left = 0.0f;
        float target_right = 0.0f;
        channel_gains(v, target_left, target_right);
        if (!v.started) {
            // A brand-new voice starts AT its target, not ramping up from zero — otherwise every
            // impact would fade in over a block and lose exactly the transient that makes it read
            // as an impact.
            v.left = target_left;
            v.right = target_right;
            v.started = true;
        }
        const float dl = (target_left - v.left) / static_cast<float>(frames);
        const float dr = (target_right - v.right) / static_cast<float>(frames);

        float gl = v.left;
        float gr = v.right;
        const std::size_t available = src.size() - std::min(src.size(), v.cursor);
        const std::size_t n = std::min(frames, available);
        for (std::size_t i = 0; i < n; ++i) {
            const float s = src[v.cursor + i];
            out[i * 2u] += s * gl;
            out[i * 2u + 1u] += s * gr;
            gl += dl;
            gr += dr;
        }
        v.cursor += n;
        v.left = target_left;
        v.right = target_right;
    }

    // Retire finished voices. Done after the loop rather than inside it because erasing
    // mid-iteration is the classic way to skip the element after the one removed.
    voices_.erase(std::remove_if(
                      voices_.begin(),
                      voices_.end(),
                      [this](const Voice& v) { return v.cursor >= bank_.samples(v.sound).size(); }),
                  voices_.end());

    // Final clamp, and COUNT what it caught. A mix that clips is not a crash and not a warning —
    // it is a distortion nobody can see, on a machine where nobody is listening. `clipped_samples`
    // is how a headless proof notices the master gain is too hot.
    for (float& s : out) {
        stats_.peak = std::max(stats_.peak, std::fabs(s));
        if (s > 1.0f) {
            s = 1.0f;
            ++stats_.clipped_samples;
        } else if (s < -1.0f) {
            s = -1.0f;
            ++stats_.clipped_samples;
        }
    }

    stats_.frames_rendered += frames;
    stats_.voices_active = voices_.size();
}

void Mixer::reset_stats() noexcept {
    const std::size_t active = voices_.size();
    stats_ = MixStats{};
    stats_.voices_active = active;
}

} // namespace rime::audio
