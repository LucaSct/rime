// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rime/audio/audio.hpp"

// The sound bank (m13.4): what a `SoundId` actually sounds like.
//
// WHY THE SOUNDS ARE SYNTHESIZED AND NOT LOADED. There is no cooked audio format — no `.rsnd`, no
// decoder, no importer — and inventing one is an asset-pipeline brick, not an audio brick. ADR-0035
// calls this slot "audio v1 … polish, cuttable", so spending it on a file format would buy a
// pipeline and no sound at all.
//
// Procedural generation gets the demo audible with no dependency, no asset, and no license entry,
// and it has a property a loaded sample does not: it is **deterministic by construction**, so the
// offline mixdown proof can assert on exact samples rather than on a checked-in binary blob nobody
// can review. When a real importer lands, `SoundBank` grows a second constructor and the mixer does
// not change — the seam is already the right shape.
//
// These are not pretending to be good sounds. They are the classic synthesis primitives for impact
// audio: a noise burst through a one-pole filter for the "crack", a decaying low sine for the
// "body", and an exponential envelope. Documented so a reader learns the recipe rather than
// inheriting a magic buffer.
namespace rime::audio {

// The engine's sample format everywhere: 32-bit float, mono in the bank (the mixer spatializes to
// stereo), 48 kHz. Float because mixing in float has no headroom cliff — you can sum a hundred
// voices past 1.0 and only care at the final clamp — and 48 kHz because it is what every modern
// device wants natively, so no resampler is needed on the way out.
inline constexpr std::uint32_t kSampleRate = 48000;

// The sounds the engine itself knows how to ask for. A game adds its own ids above `kUserBase`; the
// engine promises never to mint one there.
namespace sound {
inline constexpr SoundId kImpactConcrete = 1; // a part takes damage and survives
inline constexpr SoundId kPartBreak = 2;      // a part dies and detaches
inline constexpr SoundId kDebrisSettle = 3;   // rubble comes to rest
inline constexpr SoundId kCollapse = 4;       // a large island detaches — the low one
inline constexpr SoundId kFootstep = 5;
inline constexpr SoundId kUserBase = 1024;
} // namespace sound

// One mono sound, at kSampleRate.
struct Sound {
    std::vector<float> samples;

    [[nodiscard]] bool empty() const noexcept { return samples.empty(); }
};

// A bank of sounds addressed by id. Sparse and small: a vector indexed by id with holes is simpler
// than a map and faster to look up, and the id space the engine uses is tiny.
class SoundBank {
public:
    // Build the engine's default set — every id in `sound::` above. Deterministic: the same binary
    // always produces bit-identical buffers, because the noise source is a seeded integer hash
    // rather than anything drawn from the platform.
    [[nodiscard]] static SoundBank engine_defaults();

    void set(SoundId id, Sound sound);

    // The samples for `id`, or an empty span if the bank has nothing for it. An unknown id is
    // SILENCE, never a wrong sound — the same posture the font takes for an unprintable character.
    [[nodiscard]] std::span<const float> samples(SoundId id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return sounds_.size(); }

private:
    std::vector<Sound> sounds_;
};

// ── The synthesis primitives, exposed because they are the teaching content ──────────────────────

// A decaying noise burst: white noise through a one-pole low-pass, shaped by an exponential
// envelope. This is the "crack" of an impact — broadband, brief, and the part that carries the
// material. `cutoff01` is the filter coefficient (0 = fully damped/dull, 1 = unfiltered/bright).
[[nodiscard]] Sound
noise_burst(float seconds, float cutoff01, float decay_rate, std::uint32_t seed);

// A decaying sine: the "body" of an impact, the part that carries the size. Big things are low.
[[nodiscard]] Sound decaying_sine(float seconds, float hz, float decay_rate);

// Sum `b` into `a` at `gain`, extending `a` if needed. The bank's compound sounds are built this
// way — crack plus body — which is why they read as one hit rather than two events.
void mix_into(Sound& a, const Sound& b, float gain);

} // namespace rime::audio
