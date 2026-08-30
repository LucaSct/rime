// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The ALSA sink (m13.4). Compiled only when CMake found ALSA's development headers; otherwise
// sink_null.cpp provides `open_audio_sink` and it always returns null. Selecting the implementation
// at BUILD time rather than dlopen-ing at runtime keeps the ALSA enum constants coming from ALSA's
// own headers — hardcoding SND_PCM_FORMAT_FLOAT_LE and friends to talk to a library we did not
// include is exactly the sort of silent ABI assumption this codebase should not be making.

#include <alsa/asoundlib.h>

#include "rime/audio/sink.hpp"
#include "rime/core/diagnostics/log.hpp"

namespace rime::audio {
namespace {

class AlsaSink final : public AudioSink {
public:
    explicit AlsaSink(snd_pcm_t* pcm) noexcept : pcm_(pcm) {}

    ~AlsaSink() override {
        if (pcm_ != nullptr) {
            snd_pcm_drain(pcm_);
            snd_pcm_close(pcm_);
        }
    }

    AlsaSink(const AlsaSink&) = delete;
    AlsaSink& operator=(const AlsaSink&) = delete;

    bool write(std::span<const float> interleaved) override {
        const snd_pcm_uframes_t frames = interleaved.size() / 2u;
        if (frames == 0) {
            return true;
        }
        const snd_pcm_sframes_t written = snd_pcm_writei(pcm_, interleaved.data(), frames);
        if (written < 0) {
            // An underrun is NORMAL — it means the producer was late once, not that audio is
            // broken. `snd_pcm_recover` puts the device back and the next write continues; the
            // count is what makes a pattern of lateness visible instead of merely audible.
            ++underruns_;
            if (snd_pcm_recover(pcm_, static_cast<int>(written), /*silent=*/1) < 0) {
                RIME_WARN("audio: ALSA could not recover from {}",
                          snd_strerror(static_cast<int>(written)));
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t underruns() const noexcept override { return underruns_; }

    [[nodiscard]] const char* backend_name() const noexcept override { return "alsa"; }

private:
    snd_pcm_t* pcm_ = nullptr;
    std::uint64_t underruns_ = 0;
};

} // namespace

std::unique_ptr<AudioSink> open_audio_sink(std::uint32_t sample_rate) {
    snd_pcm_t* pcm = nullptr;
    // "default" rather than a hardware device: on a desktop that routes through PipeWire or
    // PulseAudio, which is what lets the engine share the sound card with everything else instead
    // of grabbing it exclusively.
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        RIME_INFO("audio: no ALSA playback device — running silent");
        return nullptr;
    }

    // ~40 ms of latency: low enough that an impact lands with its visual, high enough that a frame
    // hitch does not underrun. soft_resample=1 lets ALSA convert if the device dislikes 48 kHz,
    // which is better than refusing to make sound at all.
    constexpr unsigned kLatencyUs = 40'000;
    const int err = snd_pcm_set_params(pcm,
                                       SND_PCM_FORMAT_FLOAT_LE,
                                       SND_PCM_ACCESS_RW_INTERLEAVED,
                                       /*channels=*/2,
                                       sample_rate,
                                       /*soft_resample=*/1,
                                       kLatencyUs);
    if (err < 0) {
        RIME_WARN("audio: ALSA rejected the stream format ({}) — running silent",
                  snd_strerror(err));
        snd_pcm_close(pcm);
        return nullptr;
    }
    RIME_INFO("audio: ALSA sink open at {} Hz stereo", sample_rate);
    return std::make_unique<AlsaSink>(pcm);
}

} // namespace rime::audio
