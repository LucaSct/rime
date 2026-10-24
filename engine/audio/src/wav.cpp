// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/audio/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
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

namespace {

[[nodiscard]] std::uint32_t get32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

[[nodiscard]] std::uint16_t get16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

} // namespace

std::optional<WavData> read_wav(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t rate = 0;
    bool have_fmt = false;
    std::size_t data_at = 0;
    std::size_t data_len = 0;
    bool have_data = false;

    // Walk the chunk list. Chunks are word-aligned (an odd size is followed by one pad byte), and
    // each size is validated against what is actually left in the file.
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::size_t size = get32(bytes, at + 4);
        const std::size_t body = at + 8;
        if (size > bytes.size() - body) {
            if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
                // A writer that died mid-file leaves the data size too large; use what is there.
                data_at = body;
                data_len = bytes.size() - body;
                have_data = true;
            }
            break;
        }
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && size >= 16) {
            format = get16(bytes, body);
            channels = get16(bytes, body + 2);
            rate = get32(bytes, body + 4);
            bits = get16(bytes, body + 14);
            // WAVE_FORMAT_EXTENSIBLE carries the real format code in the sub-format GUID.
            if (format == 0xFFFEu && size >= 26) {
                format = get16(bytes, body + 24);
            }
            have_fmt = true;
        } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
            data_at = body;
            data_len = size;
            have_data = true;
        }
        at = body + size + (size & 1u);
    }
    if (!have_fmt || !have_data || channels == 0 || rate == 0) {
        return std::nullopt;
    }
    const bool is_pcm = format == 1u && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool is_float = format == 3u && bits == 32;
    if (!is_pcm && !is_float) {
        return std::nullopt;
    }

    const std::size_t bytes_per = bits / 8u;
    const std::size_t count = data_len / bytes_per;
    WavData out;
    out.sample_rate = rate;
    out.channels = channels;
    out.samples.resize(count - count % channels);
    for (std::size_t i = 0; i < out.samples.size(); ++i) {
        const std::size_t p = data_at + i * bytes_per;
        float v = 0.0f;
        if (is_float) {
            const std::uint32_t raw = get32(bytes, p);
            std::memcpy(&v, &raw, sizeof v);
        } else if (bits == 8) {
            v = (static_cast<float>(bytes[p]) - 128.0f) / 128.0f; // 8-bit WAV is unsigned
        } else if (bits == 16) {
            v = static_cast<float>(static_cast<std::int16_t>(get16(bytes, p))) / 32768.0f;
        } else if (bits == 24) {
            std::int32_t raw =
                static_cast<std::int32_t>((static_cast<std::uint32_t>(bytes[p]) << 8) |
                                          (static_cast<std::uint32_t>(bytes[p + 1]) << 16) |
                                          (static_cast<std::uint32_t>(bytes[p + 2]) << 24));
            v = static_cast<float>(raw >> 8) / 8388608.0f;
        } else {
            v = static_cast<float>(static_cast<std::int32_t>(get32(bytes, p))) / 2147483648.0f;
        }
        out.samples[i] = v;
    }
    return out;
}

} // namespace rime::audio
