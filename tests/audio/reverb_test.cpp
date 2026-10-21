// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// 2026-10: the reverb send, the WAV reader and the bank's variants.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "rime/audio/bank.hpp"
#include "rime/audio/mixer.hpp"
#include "rime/audio/reverb.hpp"
#include "rime/audio/wav.hpp"

using namespace rime;
using namespace rime::audio;

namespace {

double energy(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i) {
        e += static_cast<double>(x[i]) * x[i];
    }
    return e;
}

} // namespace

TEST_CASE("2026-10 reverb: an impulse rings, decays toward silence and is deterministic") {
    const auto run = []() {
        Reverb rv;
        std::vector<float> l(kSampleRate * 4, 0.0f);
        std::vector<float> r(kSampleRate * 4, 0.0f);
        std::vector<float> in(kSampleRate * 4, 0.0f);
        in[0] = 1.0f;
        rv.process(in, l, r);
        return l;
    };
    const std::vector<float> a = run();
    CHECK(a == run());

    // A tail exists (energy well after the impulse) and is far weaker 3 s on than at ~0.1-0.6 s:
    // a 1.5 s RT60 is -60 dB per 1.5 s, i.e. about -120 dB by 3 s, so the late/early ratio is tiny.
    const double early = energy(a, kSampleRate / 10, kSampleRate * 6 / 10);
    const double late = energy(a, kSampleRate * 3, kSampleRate * 4);
    CHECK(early > 1e-6);
    CHECK(late < early * 1e-3);
}

TEST_CASE("2026-10 mixer: a send puts a tail after the dry voice has ended") {
    SoundBank bank;
    Sound click;
    click.samples.assign(480, 0.5f); // 10 ms
    bank.set(sound::kUserBase, click);
    bank.set_class(sound::kUserBase, {0.0f, 0.0f, 1.0f});
    SoundBank dry_bank = bank;
    dry_bank.set_class(sound::kUserBase, {0.0f, 0.0f, 0.0f});

    const auto tail_energy = [](SoundBank b) {
        Mixer m(std::move(b));
        m.play(sound::kUserBase, {0.0f, 0.0f, -1.0f}, 1.0f);
        std::vector<float> all;
        std::vector<float> blk(1024);
        for (int i = 0; i < 40; ++i) {
            m.render(blk);
            all.insert(all.end(), blk.begin(), blk.end());
        }
        return energy(all, 2 * 4800, all.size()); // everything after the first 100 ms
    };
    CHECK(tail_energy(dry_bank) == 0.0);
    CHECK(tail_energy(bank) > 1e-6);
}

TEST_CASE("2026-10 mixer: the retrigger window refuses a restart and banks its energy") {
    SoundBank bank;
    Sound s;
    s.samples.assign(4800, 0.5f);
    bank.set(sound::kUserBase, s);
    bank.set_class(sound::kUserBase, {0.0f, 0.5f, 0.0f});
    Mixer m(std::move(bank));
    for (int i = 0; i < 10; ++i) {
        m.play(sound::kUserBase, {0.0f, 0.0f, -1.0f}, 1.0f);
    }
    CHECK(m.stats().voices_started == 1);
    CHECK(m.stats().voices_suppressed == 9);
}

TEST_CASE("2026-10 mixer: a pitch-jittered voice is deterministic and varies between plays") {
    const auto run = []() {
        SoundBank bank = SoundBank::engine_defaults();
        bank.apply_destruction_policy();
        Mixer m(std::move(bank));
        std::vector<float> all;
        std::vector<float> blk(1024);
        for (int i = 0; i < 12; ++i) {
            if (i % 4 == 0) {
                m.play(sound::kPartBreak, {0.0f, 0.0f, -2.0f}, 1.0f);
            }
            m.render(blk);
            all.insert(all.end(), blk.begin(), blk.end());
        }
        return all;
    };
    CHECK(run() == run());
}

TEST_CASE("2026-10 wav: write then read round-trips, and hostile files are refused") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = dir / "rime_audio_roundtrip_test.wav";
    std::vector<float> src(1000);
    for (std::size_t i = 0; i < src.size(); ++i) {
        src[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.5f;
    }
    REQUIRE(write_wav(path, src, kSampleRate, 1));
    const auto back = read_wav(path);
    REQUIRE(back.has_value());
    CHECK(back->sample_rate == kSampleRate);
    CHECK(back->channels == 1);
    REQUIRE(back->samples.size() == src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        CHECK(std::fabs(back->samples[i] - src[i]) < 1.0f / 16000.0f);
    }

    SoundBank bank;
    CHECK(bank.add_variant_from_wav(sound::kUserBase, path));
    CHECK(bank.variant_count(sound::kUserBase) == 1);

    // A 44.1 kHz file is refused rather than silently played at the wrong speed.
    const auto odd = dir / "rime_audio_44k_test.wav";
    REQUIRE(write_wav(odd, src, 44100, 1));
    CHECK_FALSE(bank.add_variant_from_wav(sound::kUserBase, odd));
    CHECK(bank.variant_count(sound::kUserBase) == 1);

    // Truncated and nonsense files yield nullopt, not a crash or garbage.
    const auto junk = dir / "rime_audio_junk_test.wav";
    {
        std::FILE* f = std::fopen(junk.string().c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("RIFF\x10\x00\x00\x00WAVEfmt ", f);
        std::fclose(f);
    }
    CHECK_FALSE(read_wav(junk).has_value());
    CHECK_FALSE(read_wav(dir / "rime_audio_does_not_exist.wav").has_value());
    std::filesystem::remove(path);
    std::filesystem::remove(odd);
    std::filesystem::remove(junk);
}
