// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace rime::audio {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

} // namespace

Mixer::Mixer(SoundBank bank, const MixerConfig& config)
    : bank_(std::move(bank)), config_(config), rng_(config.seed != 0 ? config.seed : 1u),
      reverb_(
          ReverbConfig{config.sample_rate, config.reverb_decay_seconds, config.reverb_damping}) {
    voices_.reserve(config_.max_voices + 1u);
}

// xorshift32: three shifts and three xors of one word of state. Not a good generator for
// statistics, a perfectly good one for "pick a variant and a few percent of pitch", and -- unlike
// std::uniform_*_distribution -- specified exactly, so the offline mixdown stays bit-identical
// across compilers.
std::uint32_t Mixer::next_random() noexcept {
    std::uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
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

void Mixer::channel_gains(const Voice& v, float& left, float& right, float& level) const noexcept {
    const core::Vec3 delta = v.position - listener_.position;
    const float distance = core::length(delta);
    const float att = attenuation(distance) * v.gain * config_.master_gain;
    level = att;

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
    const std::size_t variants = bank_.variant_count(sound);
    if (variants == 0) {
        // An id the bank has nothing for is SILENCE and a counter, never a wrong sound. Without the
        // counter this is the most invisible failure in the engine: no crash, no log, and audio
        // that is simply missing for one event type.
        ++stats_.voices_unknown;
        return;
    }
    const SoundClass& cls = bank_.sound_class(sound);

    // Two draws per play, unconditionally, so the random stream does not depend on whether this
    // particular play is later refused -- the sequence of picks stays a function of the sequence
    // of calls.
    const std::uint32_t pick = next_random();
    const std::uint32_t jitter = next_random();

    if (sound >= last_start_.size()) {
        last_start_.resize(static_cast<std::size_t>(sound) + 1u);
        last_variant_.resize(static_cast<std::size_t>(sound) + 1u, -1);
    }
    auto& starts = last_start_[sound];
    if (starts.size() < variants) {
        starts.resize(variants, std::numeric_limits<std::int64_t>::min() / 2);
    }

    // RETRIGGER GUARD. Choose a variant, but never one that started inside its class's window; scan
    // forward from the random pick to the first that is free. With four recordings a burst of a
    // hundred identical events therefore starts at most four voices per window -- different
    // recordings, so they add as independent sounds -- instead of a hundred copies of one buffer.
    const auto window =
        static_cast<std::int64_t>(cls.retrigger_seconds * static_cast<float>(config_.sample_rate));
    std::size_t variant = variants > 1 ? pick % variants : 0;
    if (variants > 1 && last_variant_[sound] == static_cast<int>(variant)) {
        variant = (variant + 1u) % variants; // never the same recording twice running
    }
    bool found = false;
    for (std::size_t tries = 0; tries < variants; ++tries) {
        if (clock_ - starts[variant] >= window) {
            found = true;
            break;
        }
        variant = (variant + 1u) % variants;
    }
    // Level of this play, computed up front: a refused play still contributes its ENERGY below.
    Voice v;
    v.sound = sound;
    v.variant = variant;
    v.position = position;
    v.gain = gain;
    v.send = cls.reverb_send;
    // Pitch: uniform in [1 - j, 1 + j] from the top 24 bits of the second draw.
    const float u = static_cast<float>(jitter >> 8) / 16777216.0f;
    v.rate = 1.0f + cls.pitch_jitter * (2.0f * u - 1.0f);
    float l = 0.0f;
    float r = 0.0f;
    channel_gains(v, l, r, v.level);
    if (!found) {
        // COALESCE rather than merely discard. A hundred part-breaks in one tick is one loud event
        // to the ear, and throwing ninety-nine away would make the biggest moment the quietest.
        // Independent sounds add as POWER (their waveforms are uncorrelated), so the refused
        // plays' squared levels are banked and the next play of this sound that is admitted starts
        // louder by sqrt(1 + banked / own^2), capped at +9.5 dB so a lone straggler cannot be
        // boosted into a roar.
        if (sound >= pending_power_.size()) {
            pending_power_.resize(static_cast<std::size_t>(sound) + 1u, 0.0f);
        }
        pending_power_[sound] += v.level * v.level;
        ++stats_.voices_suppressed;
        return;
    }
    if (sound < pending_power_.size() && pending_power_[sound] > 0.0f && v.level > 1.0e-4f) {
        const float boost =
            std::min(3.0f, std::sqrt(1.0f + pending_power_[sound] / (v.level * v.level)));
        v.gain *= boost;
        v.level *= boost;
        pending_power_[sound] = 0.0f;
    }
    v.att = v.level;
    if (v.level < 1.0e-4f) {
        // Out of earshot (the distance taper reaches zero at max_distance). Starting it would only
        // spend a voice slot a nearer sound wants.
        ++stats_.voices_inaudible;
        return;
    }

    if (voices_.size() >= config_.max_voices) {
        // The cap is full: the QUIETEST of the voices already playing is the one to lose, ranked by
        // the level it was given at play() time and refreshed every block -- never by its pan
        // gains, which are zero for a voice that has not rendered yet. (The original ranked on
        // those, so within one tick every new play evicted the previous not-yet-rendered one and a
        // 362-event burst kept only the last 48 plays of whatever came last.) If even the
        // quietest is louder than the newcomer, the newcomer is the one dropped.
        // Deterministic on ties by taking the FIRST minimum, which is also the oldest.
        auto quietest = voices_.begin();
        for (auto it = voices_.begin(); it != voices_.end(); ++it) {
            if (it->level < quietest->level) {
                quietest = it;
            }
        }
        if (v.level < quietest->level) {
            ++stats_.voices_dropped;
            return;
        }
        voices_.erase(quietest);
        ++stats_.voices_stolen;
    }

    starts[variant] = clock_;
    last_variant_[sound] = static_cast<int>(variant);
    voices_.push_back(v);
    ++stats_.voices_started;
}

void Mixer::render(std::span<float> out) {
    std::fill(out.begin(), out.end(), 0.0f);
    const std::size_t frames = out.size() / 2u;
    if (frames == 0) {
        return;
    }
    bus_.assign(frames, 0.0f);
    bool bus_used = false;

    for (Voice& v : voices_) {
        const std::span<const float> src = bank_.samples(v.sound, v.variant);
        float target_left = 0.0f;
        float target_right = 0.0f;
        float target_level = 0.0f;
        channel_gains(v, target_left, target_right, target_level);
        if (!v.started) {
            // A brand-new voice starts AT its target, not ramping up from zero — otherwise every
            // impact would fade in over a block and lose exactly the transient that makes it read
            // as an impact.
            v.left = target_left;
            v.right = target_right;
            v.att = target_level;
            v.started = true;
        }
        const float dl = (target_left - v.left) / static_cast<float>(frames);
        const float dr = (target_right - v.right) / static_cast<float>(frames);
        const float da = (target_level - v.att) / static_cast<float>(frames);
        float gl = v.left;
        float gr = v.right;
        float ga = v.att;
        const bool sends = v.send > 0.0f;
        bus_used = bus_used || sends;

        if (v.rate == 1.0f) {
            // Whole-sample path: bit-identical to the original mixer for an unjittered voice.
            const auto at = static_cast<std::size_t>(v.cursor);
            const std::size_t available = src.size() - std::min(src.size(), at);
            const std::size_t n = std::min(frames, available);
            for (std::size_t i = 0; i < n; ++i) {
                const float s = src[at + i];
                out[i * 2u] += s * gl;
                out[i * 2u + 1u] += s * gr;
                if (sends) {
                    bus_[i] += s * ga * v.send;
                }
                gl += dl;
                gr += dr;
                ga += da;
            }
            v.cursor += static_cast<double>(n);
        } else {
            // Pitch-shifted path: read the buffer at `rate` samples per output sample, linearly
            // interpolating between neighbours. Linear interpolation is the cheapest resampler and
            // its small high-frequency droop is inaudible for a +-3-5 % change on impact sounds.
            double pos = v.cursor;
            for (std::size_t i = 0; i < frames; ++i) {
                const auto idx = static_cast<std::size_t>(pos);
                if (idx >= src.size()) {
                    break;
                }
                const float frac = static_cast<float>(pos - static_cast<double>(idx));
                const float s0 = src[idx];
                const float s1 = idx + 1u < src.size() ? src[idx + 1u] : 0.0f;
                const float s = s0 + (s1 - s0) * frac;
                out[i * 2u] += s * gl;
                out[i * 2u + 1u] += s * gr;
                if (sends) {
                    bus_[i] += s * ga * v.send;
                }
                gl += dl;
                gr += dr;
                ga += da;
                pos += static_cast<double>(v.rate);
            }
            v.cursor = pos;
        }
        v.left = target_left;
        v.right = target_right;
        v.att = target_level;
        v.level = target_level;
    }

    // Retire finished voices. Done after the loop rather than inside it because erasing
    // mid-iteration is the classic way to skip the element after the one removed.
    voices_.erase(std::remove_if(voices_.begin(),
                                 voices_.end(),
                                 [this](const Voice& v) {
                                     return static_cast<std::size_t>(v.cursor) >=
                                            bank_.samples(v.sound, v.variant).size();
                                 }),
                  voices_.end());

    // The reverb. It runs while any voice is sending into it and for as long afterwards as its
    // tail audibly rings (about 1.3 x the RT60), so a shot's echo outlives the shot but a mixer
    // that never sends pays nothing.
    const auto tail_frames = static_cast<std::int64_t>(
        (config_.reverb_decay_seconds * 1.3f + 0.2f) * static_cast<float>(config_.sample_rate));
    if (bus_used) {
        tail_frames_left_ = tail_frames;
    }
    if (tail_frames_left_ > 0) {
        wet_left_.assign(frames, 0.0f);
        wet_right_.assign(frames, 0.0f);
        reverb_.process(bus_, wet_left_, wet_right_);
        for (std::size_t i = 0; i < frames; ++i) {
            out[i * 2u] += wet_left_[i] * config_.reverb_wet;
            out[i * 2u + 1u] += wet_right_[i] * config_.reverb_wet;
        }
        tail_frames_left_ -= static_cast<std::int64_t>(frames);
        if (tail_frames_left_ <= 0) {
            reverb_.clear();
        }
    }

    if (config_.limiter) {
        apply_limiter(out, frames);
    }

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
    clock_ += static_cast<std::int64_t>(frames);
    stats_.voices_active = voices_.size();
}

void Mixer::apply_limiter(std::span<float> out, std::size_t frames) {
    // LOOKAHEAD PEAK LIMITER. For every frame, `need[i]` is the gain that would bring that frame
    // exactly to the ceiling (1 if it is already under). A backward pass lets each frame's gain
    // also be no more than the next frame's gain plus a small slope, so the gain glides DOWN over
    // `kLookahead` frames ahead of a peak instead of dropping off a cliff on it -- a cliff is a
    // click. A forward pass then enforces the slow release: the gain may rise back toward 1 only a
    // little per frame. The result: peaks land on the ceiling smoothly, and the passage after a
    // loud event stays slightly ducked for ~`limiter_release_seconds`, which the ear hears as a
    // loud thing being loud rather than as distortion.
    constexpr std::size_t kLookahead = 96;
    const float ceiling = std::max(0.05f, config_.limiter_ceiling);
    limit_curve_.assign(frames + 1u, 1.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        const float m = std::max(std::fabs(out[i * 2u]), std::fabs(out[i * 2u + 1u]));
        limit_curve_[i] = m > ceiling ? ceiling / m : 1.0f;
    }
    const float slope = 1.0f / static_cast<float>(kLookahead);
    for (std::size_t i = frames; i-- > 0;) {
        limit_curve_[i] = std::min(limit_curve_[i], limit_curve_[i + 1u] + slope);
    }
    const float release =
        1.0f - std::exp(-1.0f / (std::max(0.005f, config_.limiter_release_seconds) *
                                 static_cast<float>(config_.sample_rate)));
    for (std::size_t i = 0; i < frames; ++i) {
        const float released = limiter_gain_ + (1.0f - limiter_gain_) * release;
        limiter_gain_ = std::min(limit_curve_[i], released);
        if (limiter_gain_ < 1.0f) {
            ++stats_.limited_samples;
        }
        out[i * 2u] *= limiter_gain_;
        out[i * 2u + 1u] *= limiter_gain_;
    }
}

void Mixer::reset_stats() noexcept {
    const std::size_t active = voices_.size();
    stats_ = MixStats{};
    stats_.voices_active = active;
}

} // namespace rime::audio
