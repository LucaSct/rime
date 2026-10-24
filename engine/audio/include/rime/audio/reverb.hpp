// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A small stereo reverb: the "distance" half of a gun report.
//
// WHY A GUN NEEDS ONE. A close recording of a rifle is a hard, dry crack lasting a few
// milliseconds. What tells the ear it was fired OUTDOORS, between buildings, is everything that
// comes after: the report bouncing off facades and arriving as a smeared, darkening wash a second
// or more long. Without that wash every weapon is "a click"; with it the same click is a place.
// The mixer feeds each voice into this reverb through a per-sound-class SEND, so gunfire can drive
// it hard while a pebble settling barely touches it.
//
// THE RECIPE is Schroeder's (1962) as refined by Moorer (1979) and made famous by Freeverb:
//
//   * Parallel COMB filters build the echo density. A comb is a delay line whose output is fed
//     back into its input, so one impulse comes back every `delay` samples, each repeat quieter by
//     the feedback gain. Four combs with mutually unrelated delays (so their echo trains do not
//     line up and ring at a single pitch) add together into a thick, irregular decay.
//   * Each comb's feedback passes through a ONE-POLE LOW-PASS. Air and soft surfaces absorb high
//     frequencies faster than low ones, so a real tail gets darker as it dies. Putting the filter
//     inside the feedback loop gives exactly that: every trip round the loop dulls the signal a
//     little more, so the highs vanish first.
//   * Two series ALL-PASS filters then smear the echoes in time without changing the spectrum,
//     turning the audible comb "flutter" into a diffuse wash.
//
// The comb feedback gain is chosen so each loop decays by 60 dB in `decay_seconds` (RT60):
// g = 10^(-3 * delay_seconds / RT60). That makes the decay time a physical, tunable number
// instead of a magic constant.
//
// DETERMINISM. Pure arithmetic on fixed-size state, no clock and no random source: the same input
// block sequence yields bit-identical output, which is what the offline proofs rely on. The right
// channel uses slightly longer delays (the Freeverb "stereo spread") so the two tails decorrelate
// and the wash has width.
namespace rime::audio {

struct ReverbConfig {
    std::uint32_t sample_rate = 48000;
    // Time for the tail to fall 60 dB. 1.2-1.8 s reads as an open street between buildings.
    float decay_seconds = 1.5f;
    // 0 = bright tail, 1 = very dark. Applied inside the comb feedback loops.
    float damping = 0.35f;
};

class Reverb {
public:
    explicit Reverb(const ReverbConfig& config = {});

    // Process `in` (mono) and ADD the wet signal to `left` / `right` (all the same length).
    void process(std::span<const float> in, std::span<float> left, std::span<float> right);

    // Forget all stored echoes.
    void clear() noexcept;

private:
    struct Comb {
        std::vector<float> line;
        std::size_t cursor = 0;
        float feedback = 0.0f;
        float damp_state = 0.0f;
    };

    struct AllPass {
        std::vector<float> line;
        std::size_t cursor = 0;
    };

    static constexpr std::size_t kCombs = 4;
    static constexpr std::size_t kAllPasses = 2;

    ReverbConfig config_;
    Comb combs_[2][kCombs];
    AllPass allpasses_[2][kAllPasses];
    float damp_ = 0.35f;
    // One-pole high-pass on the input: keeps low rumble out of the loops, where it would pile up
    // as mud. State only.
    float hp_in_ = 0.0f;
    float hp_out_ = 0.0f;
};

} // namespace rime::audio
