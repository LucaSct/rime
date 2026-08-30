// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/audio/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace rime::audio {
namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void put_tag(std::vector<std::uint8_t>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(tag[i]));
    }
}

} // namespace

bool write_wav(const std::filesystem::path& path,
               std::span<const float> interleaved,
               std::uint32_t sample_rate,
               std::uint32_t channels) {
    if (channels == 0 || sample_rate == 0) {
        return false;
    }
    const auto samples = static_cast<std::uint32_t>(interleaved.size());
    const std::uint32_t data_bytes = samples * 2u; // 16-bit

    // RIFF/WAVE, little-endian throughout: the 'RIFF' size counts everything after itself, which is
    // the one field people get wrong (it is file size minus 8, not the data size).
    std::vector<std::uint8_t> header;
    header.reserve(44);
    put_tag(header, "RIFF");
    put32(header, 36u + data_bytes);
    put_tag(header, "WAVE");
    put_tag(header, "fmt ");
    put32(header, 16u); // PCM fmt chunk size
    put16(header, 1u);  // format = PCM integer
    put16(header, static_cast<std::uint16_t>(channels));
    put32(header, sample_rate);
    put32(header, sample_rate * channels * 2u);               // byte rate
    put16(header, static_cast<std::uint16_t>(channels * 2u)); // block align
    put16(header, 16u);                                       // bits per sample
    put_tag(header, "data");
    put32(header, data_bytes);

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));

    std::vector<std::uint8_t> pcm;
    pcm.reserve(data_bytes);
    for (const float f : interleaved) {
        // Clamp then scale by 32767 (not 32768): the positive rail of a signed 16-bit sample is
        // 32767, and scaling by 32768 wraps a full-scale +1.0 to -32768 — a single sample of
        // maximum-amplitude opposite-sign noise, which is audible as a click.
        const float c = std::clamp(f, -1.0f, 1.0f);
        const auto v = static_cast<std::int16_t>(std::lround(c * 32767.0f));
        pcm.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) & 0xFFu));
        pcm.push_back(static_cast<std::uint8_t>((static_cast<std::uint16_t>(v) >> 8) & 0xFFu));
    }
    file.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
    return file.good();
}

} // namespace rime::audio
