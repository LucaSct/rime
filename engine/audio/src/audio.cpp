// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/audio/audio.hpp"

// The null audio backend: the whole v1 implementation. It turns play() into an append to a log the
// caller can read back — no device, no mixing. au1 replaces this file with a real backend.
namespace rime::audio {

void NullAudioBackend::play(SoundId sound, core::Vec3 position, float gain) {
    log_.push_back(Call{sound, position, gain});
}

std::span<const NullAudioBackend::Call> NullAudioBackend::log() const noexcept {
    return {log_.data(), log_.size()};
}

void NullAudioBackend::clear() noexcept {
    log_.clear();
}

} // namespace rime::audio
