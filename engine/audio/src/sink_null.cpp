// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The no-backend sink (m13.4): compiled when CMake found no ALSA headers — every non-Linux target,
// and any Linux box without libasound-dev. It reports "no device", which is an ordinary answer
// rather than a failure: the caller mixes into a buffer and writes a WAV instead. See sink.hpp.

#include "rime/audio/sink.hpp"

namespace rime::audio {

std::unique_ptr<AudioSink> open_audio_sink(std::uint32_t) {
    return nullptr;
}

} // namespace rime::audio
