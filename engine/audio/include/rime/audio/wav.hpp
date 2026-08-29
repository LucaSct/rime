// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

// Writing a mixdown to a WAV file (m13.4).
//
// This is the bridge between a proof and a human. The mixer's contract is asserted headlessly on
// exact samples, which is the right proof and tells you nothing about whether a collapse SOUNDS
// like a collapse. A WAV is how someone listens, and it costs about forty lines because the format
// is a header and then the samples — no dependency, no encoder, nothing to license.
//
// 16-bit PCM rather than float: it is what every player opens without comment, and the engine mixes
// in float precisely so that the one conversion at the very end is the only place quantization
// happens.
namespace rime::audio {

// Write interleaved float samples in [-1, 1] as a 16-bit PCM WAV. `channels` interleaves; `frames`
// is derived from the span. Values outside the range are CLAMPED, not wrapped — a wrap turns a
// slightly hot mix into a full-scale buzz, which sounds like catastrophic corruption rather than
// like the mild overshoot it is.
[[nodiscard]] bool write_wav(const std::filesystem::path& path,
                             std::span<const float> interleaved,
                             std::uint32_t sample_rate,
                             std::uint32_t channels);

} // namespace rime::audio
