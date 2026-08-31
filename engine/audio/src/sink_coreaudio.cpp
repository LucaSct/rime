// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The CoreAudio sink (macOS). Compiled only on Apple targets; ALSA serves Linux and sink_null.cpp
// serves everything else. See sink.hpp for why a sink is allowed to fail and why that is not an
// error.
//
// WHY THIS FILE EXISTS AT ALL. ADR-0035 asked for "a Linux sink first", and that is what m13.4
// built — which left macOS falling through to the null sink, so the engine was structurally deaf on
// a platform we support and develop on. The seam was already the right shape; only an
// implementation was missing.
//
// THE IMPEDANCE MISMATCH, WHICH IS THE WHOLE DESIGN PROBLEM HERE. `AudioSink::write` is a BLOCKING
// PUSH: the caller hands over frames and is held until the device has taken them, which is what
// makes the sink the playback clock. CoreAudio is the exact opposite — a PULL model, where a
// realtime thread calls a render callback and demands N frames right now. ALSA's blocking
// `snd_pcm_writei` matches the interface directly; CoreAudio cannot. Bridging them needs a buffer
// between the two threads, and that buffer's rules are the interesting part of this file:
//
//   * A single-producer/single-consumer ring. Exactly one game thread writes and exactly one audio
//     thread reads, so two monotonically increasing counters and a release/acquire pair are the
//     whole synchronisation — no mutex is ever taken by the render callback.
//   * NOTHING the callback touches may block, allocate, or lock. A render callback that waits is
//     how you get a glitch on someone's speakers: miss the deadline and the hardware plays
//     whatever was in the buffer. So the callback only loads/stores atomics and memcpys, and when
//     it is short it writes SILENCE and counts an underrun rather than waiting for more.
//   * The writer blocks by SLEEPING in small slices rather than waiting on a condition variable
//     the callback would have to signal. Signalling a condvar from a realtime thread can take a
//     lock inside the runtime, which is the one thing that thread must not do. Polling costs a few
//     wakeups per buffer and keeps the callback provably lock-free — a good trade at 40 ms.
//
// The ring's capacity IS the latency. The writer only blocks when the ring is full, so in steady
// state the ring sits near-full and playback trails the mixer by roughly its depth. 40 ms matches
// the figure the ALSA sink asks the device for, for the same reason: low enough that an impact
// lands with its visual, high enough to survive a frame hitch.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "rime/audio/sink.hpp"
#include "rime/core/diagnostics/log.hpp"

namespace rime::audio {
namespace {

constexpr std::uint32_t kChannels = 2;

// Ring depth, and so the sink's latency. See the header note.
constexpr std::uint32_t kLatencyMs = 40;

// How long the writer sleeps when the ring is full. A fraction of the buffer depth: short enough
// that the device never starves while we are asleep, long enough not to spin a core.
constexpr auto kWriterPollInterval = std::chrono::microseconds(500);

// How long the writer will wait for a ring that never drains before declaring the device dead.
// A blocking push API with no upper bound is a hang waiting for someone to unplug their headphones:
// if the unit stops calling us back, `write` must return false rather than sleep forever. Generous,
// because the only thing that legitimately takes this long is a device switch.
constexpr auto kWriterStallTimeout = std::chrono::seconds(2);

class CoreAudioSink final : public AudioSink {
public:
    CoreAudioSink(AudioUnit unit, std::size_t capacity_samples, bool interleaved) noexcept
        : unit_(unit), ring_(capacity_samples, 0.0f), interleaved_(interleaved) {}

    ~CoreAudioSink() override {
        if (unit_ != nullptr) {
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
        }
    }

    CoreAudioSink(const CoreAudioSink&) = delete;
    CoreAudioSink& operator=(const CoreAudioSink&) = delete;

    bool write(std::span<const float> interleaved) override {
        std::size_t offset = 0;
        auto stalled_since = std::chrono::steady_clock::now();
        while (offset < interleaved.size()) {
            const std::size_t w = write_.load(std::memory_order_relaxed);
            const std::size_t r = read_.load(std::memory_order_acquire);
            const std::size_t space = ring_.size() - (w - r);
            if (space == 0) {
                // The ring is full, which is the ordinary steady state: the device has not yet
                // consumed what we already queued. Sleeping here is what paces the caller's loop.
                if (std::chrono::steady_clock::now() - stalled_since > kWriterStallTimeout) {
                    RIME_WARN("audio: CoreAudio stopped consuming — the device is gone");
                    return false;
                }
                std::this_thread::sleep_for(kWriterPollInterval);
                continue;
            }
            stalled_since = std::chrono::steady_clock::now();
            const std::size_t n = std::min(space, interleaved.size() - offset);
            const std::size_t start = w % ring_.size();
            const std::size_t first = std::min(n, ring_.size() - start);
            std::memcpy(ring_.data() + start, interleaved.data() + offset, first * sizeof(float));
            if (n > first) {
                std::memcpy(
                    ring_.data(), interleaved.data() + offset + first, (n - first) * sizeof(float));
            }
            // Release: the samples above must be visible to the render thread before the index
            // that publishes them is.
            write_.store(w + n, std::memory_order_release);
            offset += n;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t underruns() const noexcept override {
        return underruns_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const char* backend_name() const noexcept override { return "coreaudio"; }

    // The render callback. Runs on CoreAudio's realtime thread — see the file header for what it is
    // forbidden from doing.
    static OSStatus render(void* ref,
                           AudioUnitRenderActionFlags* flags,
                           const AudioTimeStamp* /*timestamp*/,
                           UInt32 /*bus*/,
                           UInt32 frames,
                           AudioBufferList* io) {
        auto* self = static_cast<CoreAudioSink*>(ref);
        if (io == nullptr || io->mNumberBuffers == 0) {
            return noErr; // nothing to fill; never dereference a buffer list we were not given
        }

        // Believe the buffer list's own size rather than `frames`. They agree in practice, but this
        // runs on the realtime thread where being wrong is a crash inside CoreAudio rather than a
        // failed assertion, and the clamp costs one divide.
        const std::size_t buffer_floats = io->mBuffers[0].mDataByteSize / sizeof(float);
        const std::size_t requested = static_cast<std::size_t>(frames) * kChannels;
        const std::size_t wanted = self->interleaved_
                                       ? std::min(requested, buffer_floats)
                                       : std::min(requested, buffer_floats * kChannels);

        const std::size_t r = self->read_.load(std::memory_order_relaxed);
        const std::size_t available = self->write_.load(std::memory_order_acquire) - r;
        // Whole frames only. A caller that handed us an odd number of samples must not make us
        // emit half a frame and shift every channel afterwards.
        const std::size_t take = (std::min(available, wanted) / kChannels) * kChannels;
        if (take < wanted && self->write_.load(std::memory_order_relaxed) > 0) {
            // The producer was late. Not an error and not recoverable by waiting — the deadline is
            // now. Play what we have, pad with silence, and count it so a pattern of lateness is
            // visible rather than merely audible (the same reasoning as the ALSA sink's recover).
            self->underruns_.fetch_add(1, std::memory_order_relaxed);
        }

        if (self->interleaved_) {
            auto* out = static_cast<float*>(io->mBuffers[0].mData);
            self->copy_out(r, take, [out](std::size_t i, float v) { out[i] = v; });
            std::fill(out + take, out + wanted, 0.0f);
        } else {
            // Non-interleaved: one buffer per channel, so de-interleave on the way out.
            const UInt32 buffers = io->mNumberBuffers;
            self->copy_out(r, take, [io, buffers](std::size_t i, float v) {
                const UInt32 channel = static_cast<UInt32>(i % kChannels);
                if (channel < buffers) {
                    static_cast<float*>(io->mBuffers[channel].mData)[i / kChannels] = v;
                }
            });
            for (UInt32 b = 0; b < buffers; ++b) {
                auto* out = static_cast<float*>(io->mBuffers[b].mData);
                std::fill(out + take / kChannels, out + wanted / kChannels, 0.0f);
            }
        }

        self->read_.store(r + take, std::memory_order_release);
        if (take == 0 && flags != nullptr) {
            // Tell the unit this buffer is silence, so downstream can skip work.
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
        }
        return noErr;
    }

private:
    // Walk `count` samples out of the ring starting at absolute index `from`, handing each to
    // `emit` with its offset within this callback's request. Wrapping lives here, once.
    template <typename Emit> void copy_out(std::size_t from, std::size_t count, Emit emit) const {
        for (std::size_t i = 0; i < count; ++i) {
            emit(i, ring_[(from + i) % ring_.size()]);
        }
    }

    AudioUnit unit_ = nullptr;
    std::vector<float> ring_;
    bool interleaved_ = true;

    // Absolute sample counters, never wrapped: their DIFFERENCE is the fill level, which stays
    // correct across the modulo used to index the ring.
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
    std::atomic<std::uint64_t> underruns_{0};
};

} // namespace

std::unique_ptr<AudioSink> open_audio_sink(std::uint32_t sample_rate) {
    // The DEFAULT output unit, not a specific device: it follows the user's output selection and
    // survives them switching to headphones mid-session, which is the macOS equivalent of ALSA's
    // "default" going through PipeWire rather than grabbing the card.
    AudioComponentDescription want{};
    want.componentType = kAudioUnitType_Output;
    want.componentSubType = kAudioUnitSubType_DefaultOutput;
    want.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &want);
    if (component == nullptr) {
        RIME_INFO("audio: no CoreAudio default output component — running silent");
        return nullptr;
    }

    AudioUnit unit = nullptr;
    if (AudioComponentInstanceNew(component, &unit) != noErr || unit == nullptr) {
        RIME_INFO("audio: could not open the CoreAudio default output — running silent");
        return nullptr;
    }

    // Ask for interleaved stereo float, which is what AudioSink::write hands us. The unit has a
    // converter in front of it and normally accepts this; we do not assume it did, though — the
    // format is read BACK below, because a sink that guessed the layout would write channel data
    // into the wrong buffer and the failure would be audible rather than diagnosable.
    AudioStreamBasicDescription format{};
    format.mSampleRate = static_cast<Float64>(sample_rate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = sizeof(float) * kChannels;
    format.mBytesPerPacket = format.mBytesPerFrame;

    if (AudioUnitSetProperty(unit,
                             kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input,
                             0,
                             &format,
                             sizeof(format)) != noErr) {
        RIME_WARN("audio: CoreAudio rejected interleaved stereo float — running silent");
        AudioComponentInstanceDispose(unit);
        return nullptr;
    }

    AudioStreamBasicDescription actual{};
    UInt32 actual_size = sizeof(actual);
    if (AudioUnitGetProperty(unit,
                             kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input,
                             0,
                             &actual,
                             &actual_size) != noErr) {
        RIME_WARN("audio: could not read back the CoreAudio stream format — running silent");
        AudioComponentInstanceDispose(unit);
        return nullptr;
    }
    // kAudioFormatFlagIsNonInterleaved is the flag's own spelling of "one buffer per channel".
    const bool interleaved = (actual.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;

    // Size the ring from the rate the unit actually settled on, not the one we asked for.
    const auto effective_rate = static_cast<std::uint32_t>(
        actual.mSampleRate > 0.0 ? actual.mSampleRate : static_cast<Float64>(sample_rate));
    // At least one frame: ring_.size() is a modulus, and a rate low enough to round the latency
    // window down to nothing would divide by zero rather than merely sound bad.
    const std::size_t capacity =
        std::max<std::size_t>(static_cast<std::size_t>(effective_rate) * kLatencyMs / 1000u, 1u) *
        kChannels;

    auto sink = std::make_unique<CoreAudioSink>(unit, capacity, interleaved);

    AURenderCallbackStruct callback{};
    callback.inputProc = &CoreAudioSink::render;
    callback.inputProcRefCon = sink.get();
    if (AudioUnitSetProperty(unit,
                             kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input,
                             0,
                             &callback,
                             sizeof(callback)) != noErr) {
        RIME_WARN("audio: CoreAudio refused the render callback — running silent");
        return nullptr; // the sink's destructor disposes the unit
    }

    if (AudioUnitInitialize(unit) != noErr) {
        RIME_INFO("audio: no CoreAudio playback device — running silent");
        return nullptr;
    }
    if (AudioOutputUnitStart(unit) != noErr) {
        RIME_INFO("audio: CoreAudio would not start playback — running silent");
        return nullptr;
    }

    RIME_INFO("audio: CoreAudio sink open at {} Hz stereo ({})",
              effective_rate,
              interleaved ? "interleaved" : "planar");
    return sink;
}

} // namespace rime::audio
