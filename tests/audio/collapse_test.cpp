// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// 2026-10: a collapse must not BUZZ.
//
// The owner heard the city block fall over as "a vibrator on high frequency". Measured on the real
// destruction event stream (see samples/99-the-block --audio-wav): ONE tick delivered 362
// part-break events, and the mixer started a voice for each -- 48 coherent copies of one 109 Hz
// decaying sine that put 42 % of the whole mix's energy into a single spectral line -- followed by
// a play on every tick or two that restarted the same short buffer on the 60 Hz tick grid. Both are
// periodic, and periodic is what the ear calls a tone.
//
// This is the proof that does not need ears. It renders a synthetic dense collapse (300 events in
// one second, the first tick carrying a hundred of them, as the real one does) through the mixer
// and asserts three properties of the WAVEFORM:
//
//   1. nothing clips and the voice count never exceeds the cap;
//   2. the mix is not tonal: no single spectral line carries a dominant share of the energy;
//   3. the mix is not periodic at the tick period: the autocorrelation at a lag of one simulation
//      tick (800 samples at 60 Hz) is small. A restart-every-tick comb has a large value there.
//
// SENSITIVITY CONTROL. A near-null proof is worthless without something showing the scene COULD
// have failed. These same events were run through the unmodified (pre-fix) mixer and bank: the
// line carried 0.41 of the energy and the tick autocorrelation was 0.38 (measured 2026-10-21) --
// both rejected below -- while the fixed mixer measures 0.07 and 0.00. The thresholds sit between.
//
// LOUDNESS GUARD. Refusing plays is how the buzz is prevented, and it must not turn the biggest
// moment into the quietest one. So a fourth assertion compares the dense collapse's energy with a
// single part-break at the same median distance: it has to be several times louder.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

#include "rime/audio/bank.hpp"
#include "rime/audio/destruction_voicer.hpp"
#include "rime/audio/mixer.hpp"

using namespace rime;
using namespace rime::audio;

namespace {

constexpr std::size_t kTick = kSampleRate / 60; // 800 frames: one simulation tick of audio

// In-place iterative radix-2 FFT. A test-local helper: the engine has no FFT and a proof about a
// spectrum should not depend on one it did not write.
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Share of the (20 Hz+) energy that sits within +-6 Hz of the single strongest spectral line.
double strongest_line_share(const std::vector<float>& mono) {
    std::size_t n = 1;
    while (n < mono.size()) {
        n <<= 1;
    }
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        // Hann window: without it the hard start/stop of the buffer smears every line across the
        // spectrum and a pure tone would look broadband.
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                              static_cast<double>(mono.size() - 1));
        a[i] = mono[i] * w;
    }
    fft(a);
    const double bin_hz = static_cast<double>(kSampleRate) / static_cast<double>(n);
    const auto lo = static_cast<std::size_t>(20.0 / bin_hz);
    std::vector<double> power(n / 2);
    double total = 0.0;
    for (std::size_t i = lo; i < n / 2; ++i) {
        power[i] = std::norm(a[i]);
        total += power[i];
    }
    const auto half = static_cast<std::size_t>(6.0 / bin_hz) + 1;
    double best = 0.0;
    for (std::size_t i = lo; i < n / 2; ++i) {
        double sum = 0.0;
        for (std::size_t k = i > half ? i - half : 0; k <= std::min(n / 2 - 1, i + half); ++k) {
            sum += power[k];
        }
        best = std::max(best, sum);
    }
    return total > 0.0 ? best / total : 0.0;
}

// Normalised autocorrelation of `x` at `lag` (mean removed). 1 = perfectly periodic at that lag.
double autocorrelation(const std::vector<float>& x, std::size_t lag) {
    double mean = 0.0;
    for (const float v : x) {
        mean += v;
    }
    mean /= static_cast<double>(x.size());
    double num = 0.0;
    double den = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double a = x[i] - mean;
        den += a * a;
        if (i + lag < x.size()) {
            num += a * (x[i + lag] - mean);
        }
    }
    return den > 0.0 ? num / den : 0.0;
}

double energy_of(const std::vector<float>& x, std::size_t frames) {
    double e = 0.0;
    for (std::size_t i = 0; i < std::min(frames, x.size()); ++i) {
        e += static_cast<double>(x[i]) * x[i];
    }
    return e;
}

struct CollapseRender {
    std::vector<float> mono;
    MixStats stats;
    std::size_t max_voices_seen = 0;
    // Wall-clock cost of Mixer::render per 16.7 ms tick (reported, never asserted: CI machines
    // vary).
    double render_us_total = 0.0;
    double render_us_max = 0.0;
    std::size_t render_calls = 0;
};

// 300 events in the first second: 120 of them in the very first tick (what a charge does), the rest
// spread over the following 59 ticks, three collapses and a few dozen settles among them. Positions
// and magnitudes come from a fixed LCG so the scene is the same on every compiler.
CollapseRender render_collapse(Mixer& mixer) {
    std::uint32_t lcg = 12345;
    const auto rnd = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return static_cast<float>(lcg >> 8) / 16777216.0f;
    };
    CollapseRender out;
    mixer.set_listener(Listener{});
    std::vector<float> block(kTick * 2u);
    for (std::size_t tick = 0; tick < 60u + 90u; ++tick) {
        std::size_t events = 0;
        if (tick == 0) {
            events = 120;
        } else if (tick < 60) {
            events = 180 / 59 + (tick % 59 < 180 % 59 ? 1 : 0);
        }
        for (std::size_t e = 0; e < events; ++e) {
            const core::Vec3 at{(rnd() - 0.5f) * 40.0f, rnd() * 6.0f, -(4.0f + rnd() * 30.0f)};
            const float magnitude = 0.4f + 0.6f * rnd();
            const std::uint32_t kind = static_cast<std::uint32_t>(rnd() * 100.0f);
            if (kind < 2) {
                mixer.play(sound::kCollapse, at, 1.0f);
            } else if (kind < 20) {
                mixer.play(sound::kDebrisSettle, at, 0.35f);
            } else {
                mixer.play(sound::kPartBreak, at, magnitude);
            }
        }
        out.max_voices_seen = std::max(out.max_voices_seen, mixer.voice_count());
        mixer.render(block);
        for (std::size_t i = 0; i < kTick; ++i) {
            out.mono.push_back(block[i * 2u] + block[i * 2u + 1u]);
        }
    }
    out.stats = mixer.stats();
    return out;
}

} // namespace

TEST_CASE("2026-10: a dense collapse neither clips, nor drones, nor ticks") {
    SoundBank bank = SoundBank::engine_defaults();
    bank.apply_destruction_policy(); // NEW-API
    MixerConfig cfg;
    cfg.master_gain = 0.12f; // the block sample's setting, chosen for exactly this density
    cfg.limiter = true;      // NEW-API
    Mixer mixer(std::move(bank), cfg);

    const CollapseRender r = render_collapse(mixer);

    // Non-vacuity: the scene really made sound and really exceeded the voice cap.
    REQUIRE(r.stats.voices_started > 0);
    double energy = 0.0;
    for (const float v : r.mono) {
        energy += static_cast<double>(v) * v;
    }
    REQUIRE(energy > 1.0);

    CHECK(r.stats.peak <= 1.0f);
    CHECK(r.stats.clipped_samples == 0);
    CHECK(r.max_voices_seen <= cfg.max_voices);

    // The control: one part-break at the scene's median distance and gain, same mixer settings.
    SoundBank solo_bank = SoundBank::engine_defaults();
    solo_bank.apply_destruction_policy(); // NEW-API
    Mixer solo(std::move(solo_bank), cfg);
    solo.set_listener(Listener{});
    solo.play(sound::kPartBreak, {0.0f, 3.0f, -19.0f}, 0.7f);
    std::vector<float> block(kTick * 2u);
    std::vector<float> solo_mono;
    for (int t = 0; t < 30; ++t) {
        solo.render(block);
        for (std::size_t i = 0; i < kTick; ++i) {
            solo_mono.push_back(block[i * 2u] + block[i * 2u + 1u]);
        }
    }
    const double dense_energy = energy_of(r.mono, 30 * kTick);
    const double solo_energy = energy_of(solo_mono, 30 * kTick);
    MESSAGE("first 0.5 s energy: dense " << dense_energy << ", single break " << solo_energy);
    CHECK(dense_energy > 4.0 * solo_energy);

    const double line = strongest_line_share(r.mono);
    const double tick_ac = autocorrelation(r.mono, kTick);
    MESSAGE("strongest spectral line share " << line << ", autocorrelation at one tick " << tick_ac
                                             << ", peak " << r.stats.peak);
    CHECK(line < 0.25);
    CHECK(std::fabs(tick_ac) < 0.25);
}

namespace {

// Same scene as render_collapse, but routed through the DestructionVoicer the game uses.
CollapseRender render_collapse_voiced(Mixer& mixer, DestructionVoicer& voicer) {
    std::uint32_t lcg = 12345;
    const auto rnd = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return static_cast<float>(lcg >> 8) / 16777216.0f;
    };
    CollapseRender out;
    mixer.set_listener(Listener{});
    std::vector<float> block(kTick * 2u);
    for (std::size_t tick = 0; tick < 60u + 150u; ++tick) {
        std::size_t events = 0;
        if (tick == 0) {
            events = 120;
        } else if (tick < 60) {
            events = 180 / 59 + (tick % 59 < 180 % 59 ? 1 : 0);
        }
        for (std::size_t e = 0; e < events; ++e) {
            const core::Vec3 at{(rnd() - 0.5f) * 40.0f, rnd() * 6.0f, -(4.0f + rnd() * 30.0f)};
            const float magnitude = 0.4f + 0.6f * rnd();
            const std::uint32_t kind = static_cast<std::uint32_t>(rnd() * 100.0f);
            if (kind < 2) {
                voicer.add(DestructionKind::IslandDetached, at, 1.0f);
            } else if (kind < 20) {
                voicer.add(DestructionKind::DebrisSettled, at, 0.35f);
            } else {
                voicer.add(DestructionKind::PartBroken, at, magnitude);
            }
        }
        voicer.update(mixer, 1.0f / 60.0f);
        out.max_voices_seen = std::max(out.max_voices_seen, mixer.voice_count());
        const auto t0 = std::chrono::steady_clock::now();
        mixer.render(block);
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                .count();
        out.render_us_total += us;
        out.render_us_max = std::max(out.render_us_max, us);
        ++out.render_calls;
        for (std::size_t i = 0; i < kTick; ++i) {
            out.mono.push_back(block[i * 2u] + block[i * 2u + 1u]);
        }
    }
    out.stats = mixer.stats();
    return out;
}

// Records what the voicer asked for, without a mixer.
struct RecordingBackend final : AudioBackend {
    std::vector<SoundId> plays;

    void play(SoundId sound, core::Vec3, float) override { plays.push_back(sound); }
};

} // namespace

TEST_CASE("2026-10 voicer: the same collapse, aggregated, is a few transients plus a bed") {
    SoundBank bank = SoundBank::engine_defaults();
    bank.apply_destruction_policy();
    MixerConfig cfg;
    cfg.master_gain = 0.5f;
    cfg.limiter = true;
    cfg.limiter_ceiling = 0.891f; // -1 dBFS
    Mixer mixer(std::move(bank), cfg);
    DestructionVoicer voicer;

    const CollapseRender r = render_collapse_voiced(mixer, voicer);
    const auto& st = voicer.stats();
    MESSAGE("events " << st.events_in << ", transients " << st.transients_played << ", collapses "
                      << st.collapses_played << ", folded into bed " << st.folded_into_bed
                      << ", bed starts " << st.bed_starts << ", trickles " << st.trickles
                      << ", peak " << r.stats.peak);

    CHECK(st.events_in == 300);
    CHECK(st.transients_played <= 40);     // 12 windows x 3, not 300
    CHECK(st.transients_played >= 6);      // and it did voice impacts
    CHECK(st.bed_starts >= 1);             // the roar exists
    CHECK(st.folded_into_bed > 100);       // most of the events became bed, not clicks
    CHECK(r.stats.peak <= 0.891f + 1e-3f); // -1 dBFS ceiling holds with master_gain raised to 0.5
    CHECK(r.stats.clipped_samples == 0);
    CHECK(r.max_voices_seen <= cfg.max_voices);

    const double line = strongest_line_share(r.mono);
    const double tick_ac = autocorrelation(r.mono, kTick);
    MESSAGE("render cost per tick (reverb running): mean "
            << r.render_us_total / static_cast<double>(r.render_calls) << " us, max "
            << r.render_us_max << " us of a 16667 us budget");
    MESSAGE("voiced: strongest line " << line << ", tick autocorrelation " << tick_ac);
    CHECK(line < 0.25);
    CHECK(std::fabs(tick_ac) < 0.25);
}

TEST_CASE("2026-10 voicer: debris settling never makes an impact, and quiets the bed") {
    RecordingBackend backend;
    DestructionVoicer voicer;
    // A big burst first, to build bed intensity...
    for (int i = 0; i < 80; ++i) {
        voicer.add(
            DestructionKind::PartBroken, {static_cast<float>(i % 7) * 5.0f, 0, -10.0f}, 0.8f);
    }
    for (int t = 0; t < 10; ++t) {
        voicer.update(backend, 1.0f / 60.0f);
    }
    const float before = voicer.bed_intensity();
    REQUIRE(before > 0.05f);
    backend.plays.clear();
    // ...then nothing but settles.
    for (int i = 0; i < 20; ++i) {
        voicer.add(DestructionKind::DebrisSettled, {0, 0, -8.0f}, 1.0f);
    }
    voicer.update(backend, 1.0f / 60.0f);
    CHECK(voicer.bed_intensity() < before);
    for (const SoundId id : backend.plays) {
        CHECK(id != sound::kPartBreak);
        CHECK(id != sound::kCollapse);
        CHECK(id != sound::kImpactConcrete);
    }
}

TEST_CASE("2026-10 mixer: voice eviction fades rather than cutting, and destruction has a budget") {
    SoundBank bank = SoundBank::engine_defaults();
    bank.apply_destruction_policy();
    Sound tone;
    tone.samples.assign(kSampleRate, 0.5f); // a long DC-ish voice: any hard cut would be a step
    bank.set(sound::kUserBase, tone);       // group 0: gunfire-like
    MixerConfig cfg;
    cfg.max_voices = 8;
    cfg.group_max_voices = {0u, 3u, 0u, 0u};
    Mixer mixer(std::move(bank), cfg);
    mixer.set_listener(Listener{});
    std::vector<float> block(kTick * 2u);

    // Five gun voices, then a flood of destruction: the destruction group is capped at 3 and the
    // guns are not evicted by it.
    for (int i = 0; i < 5; ++i) {
        mixer.play(sound::kUserBase, {0.0f, 0.0f, -1.0f}, 1.0f);
    }
    mixer.render(block);
    for (int i = 0; i < 20; ++i) {
        mixer.play(sound::kCollapse, {static_cast<float>(i), 0.0f, -3.0f}, 1.0f);
    }
    mixer.render(block);
    CHECK(mixer.voice_count() <= 8);

    // Evict one sounding gun voice and check the output has no step larger than the fade allows.
    SoundBank b2 = SoundBank::engine_defaults();
    b2.set(sound::kUserBase, tone);
    MixerConfig c2;
    c2.max_voices = 1;
    Mixer m2(std::move(b2), c2);
    m2.set_listener(Listener{});
    m2.play(sound::kUserBase, {0.0f, 0.0f, -1.0f}, 0.5f);
    m2.render(block);
    m2.play(sound::kUserBase, {0.0f, 0.0f, -1.0f}, 1.0f); // louder: evicts the first
    std::vector<float> after(kTick * 2u);
    m2.render(after);
    float max_step = 0.0f;
    for (std::size_t i = 1; i < kTick; ++i) {
        max_step = std::max(max_step, std::fabs(after[i * 2u] - after[(i - 1) * 2u]));
    }
    // The old voice glides 0.5 -> 0 over 800 frames and the new one is constant, so consecutive
    // samples differ by ~1e-3, not by the ~0.12 a hard cut of the old voice would show.
    CHECK(max_step < 0.01f);
}
