// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/stream/frame_codec.hpp"

#include <lz4.h>
#include <turbojpeg.h>

#include <cstring>
#include <utility>

#include "rime/core/diagnostics/log.hpp"

// The codec's implementation. Each codec is a thin, honest wrapper over a well-worn library call;
// the teaching is in *which* codec and *why* (see the header and ADR-0017), not in re-deriving
// JPEG. The two libraries are confined to this translation unit — the public header speaks only
// std::byte and opaque void* handles — so no consumer of rime::stream inherits
// <turbojpeg.h>/<lz4.h>.
namespace rime::stream {
namespace {

// Map an RHI 8-bit colour format to TurboJPEG's pixel layout. Returns TJPF_UNKNOWN (-1) for
// anything we don't stream. RGBA/BGRA differ only in byte order; TurboJPEG reads whichever we name.
int tj_pixel_format(rhi::Format format) {
    switch (format) {
        case rhi::Format::RGBA8Unorm:
        case rhi::Format::RGBA8Srgb:
            return TJPF_RGBA;
        case rhi::Format::BGRA8Unorm:
        case rhi::Format::BGRA8Srgb:
            return TJPF_BGRA;
        default:
            return TJPF_UNKNOWN;
    }
}

// v0 chroma subsampling: 4:2:0 stores luma per pixel but chroma for each 2x2 block, ~2x smaller
// than 4:4:4 for a loss the eye barely catches on photographic / shaded-3-D content (it does show
// on high-contrast UI text — a per-content choice S1 can make). Fixed for v0 so the wire stays
// simple.
constexpr int kJpegSubsamp = TJSAMP_420;

// TurboJPEG source pitch: 0 tells it "rows are tightly packed" (stride = width x pixel size), which
// is exactly how FrameStreamer lays pixels out. Avoids hard-coding the 4-byte pixel size here.
constexpr int kTightlyPacked = 0;

} // namespace

bool is_supported_format(rhi::Format format) noexcept {
    return tj_pixel_format(format) != TJPF_UNKNOWN;
}

// ── FrameEncoder ────────────────────────────────────────────────────────────────────────────────

bool FrameEncoder::encode(Codec codec,
                          const ImageDesc& desc,
                          std::span<const std::byte> pixels,
                          std::vector<std::byte>& out) {
    out.clear();

    if (!is_supported_format(desc.format)) {
        RIME_ERROR("FrameEncoder: unsupported format (S0 codecs speak 8-bit RGBA/BGRA only)");
        return false;
    }
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        RIME_ERROR("FrameEncoder: zero extent ({}x{})", desc.extent.width, desc.extent.height);
        return false;
    }
    if (pixels.size() != desc.byte_size()) {
        RIME_ERROR("FrameEncoder: got {} pixel bytes, expected {} for {}x{}",
                   pixels.size(),
                   desc.byte_size(),
                   desc.extent.width,
                   desc.extent.height);
        return false;
    }

    switch (codec) {
        case Codec::Raw: {
            // The baseline. Copy through so callers have one uniform "encode then send" path even
            // when they choose not to compress (loopback, or the benchmark's reference point).
            out.resize(pixels.size());
            std::memcpy(out.data(), pixels.data(), pixels.size());
            return true;
        }

        case Codec::LZ4: {
            // Lossless and nearly free. LZ4_compressBound() gives the worst-case output size (the
            // input can *grow* slightly if it is incompressible — e.g. noise), so we size to that,
            // compress, then shrink to what was actually written.
            if (pixels.size() > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
                RIME_ERROR("FrameEncoder: frame too large for LZ4 ({} bytes)", pixels.size());
                return false;
            }
            const int src_size = static_cast<int>(pixels.size());
            const int cap = LZ4_compressBound(src_size);
            out.resize(static_cast<std::size_t>(cap));
            const int n = LZ4_compress_default(reinterpret_cast<const char*>(pixels.data()),
                                               reinterpret_cast<char*>(out.data()),
                                               src_size,
                                               cap);
            if (n <= 0) {
                RIME_ERROR("FrameEncoder: LZ4_compress_default failed");
                out.clear();
                return false;
            }
            out.resize(static_cast<std::size_t>(n));
            return true;
        }

        case Codec::Jpeg: {
            // Reuse one compressor handle across the stream (created on first use) — tjInitCompress
            // is not free, and a streamer encodes thousands of frames.
            if (jpeg_ == nullptr) {
                jpeg_ = tjInitCompress();
                if (jpeg_ == nullptr) {
                    RIME_ERROR("FrameEncoder: tjInitCompress failed: {}", tjGetErrorStr());
                    return false;
                }
            }
            const auto handle = static_cast<tjhandle>(jpeg_);
            const int w = static_cast<int>(desc.extent.width);
            const int h = static_cast<int>(desc.extent.height);
            const int pf = tj_pixel_format(desc.format);

            // Pre-size `out` to the worst-case JPEG size and pass TJFLAG_NOREALLOC: libjpeg-turbo
            // then writes straight into our vector and never reallocates, so there is no tjAlloc
            // buffer to copy out of and free. It sets jpeg_size to the real length, which we shrink
            // to.
            out.resize(static_cast<std::size_t>(tjBufSize(w, h, kJpegSubsamp)));
            auto* dst = reinterpret_cast<unsigned char*>(out.data());
            unsigned long jpeg_size = 0;
            const int rc = tjCompress2(handle,
                                       reinterpret_cast<const unsigned char*>(pixels.data()),
                                       w,
                                       kTightlyPacked,
                                       h,
                                       pf,
                                       &dst,
                                       &jpeg_size,
                                       kJpegSubsamp,
                                       options_.jpeg_quality,
                                       TJFLAG_NOREALLOC);
            if (rc != 0) {
                RIME_ERROR("FrameEncoder: tjCompress2 failed: {}", tjGetErrorStr2(handle));
                out.clear();
                return false;
            }
            out.resize(static_cast<std::size_t>(jpeg_size));
            return true;
        }
    }

    RIME_ERROR("FrameEncoder: unknown codec {}", static_cast<int>(codec));
    return false;
}

FrameEncoder::~FrameEncoder() {
    if (jpeg_ != nullptr) {
        tjDestroy(static_cast<tjhandle>(jpeg_));
    }
}

FrameEncoder::FrameEncoder(FrameEncoder&& other) noexcept
    : options_(other.options_), jpeg_(other.jpeg_) {
    other.jpeg_ = nullptr;
}

FrameEncoder& FrameEncoder::operator=(FrameEncoder&& other) noexcept {
    if (this != &other) {
        if (jpeg_ != nullptr) {
            tjDestroy(static_cast<tjhandle>(jpeg_));
        }
        options_ = other.options_;
        jpeg_ = other.jpeg_;
        other.jpeg_ = nullptr;
    }
    return *this;
}

// ── FrameDecoder ────────────────────────────────────────────────────────────────────────────────

bool FrameDecoder::decode(Codec codec,
                          const ImageDesc& desc,
                          std::span<const std::byte> in,
                          std::span<std::byte> out) {
    if (!is_supported_format(desc.format)) {
        RIME_ERROR("FrameDecoder: unsupported format (S0 codecs speak 8-bit RGBA/BGRA only)");
        return false;
    }
    if (out.size() != desc.byte_size()) {
        RIME_ERROR("FrameDecoder: output span is {} bytes, expected {} for {}x{}",
                   out.size(),
                   desc.byte_size(),
                   desc.extent.width,
                   desc.extent.height);
        return false;
    }
    if (in.empty()) {
        RIME_ERROR("FrameDecoder: empty input");
        return false;
    }

    switch (codec) {
        case Codec::Raw: {
            if (in.size() != out.size()) {
                RIME_ERROR(
                    "FrameDecoder: raw input is {} bytes, expected {}", in.size(), out.size());
                return false;
            }
            std::memcpy(out.data(), in.data(), out.size());
            return true;
        }

        case Codec::LZ4: {
            // _safe never reads past `in` nor writes past `out` even if the input is corrupt — the
            // decoder half of trusting-nothing. A valid frame must fill exactly out.size() bytes.
            const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(in.data()),
                                              reinterpret_cast<char*>(out.data()),
                                              static_cast<int>(in.size()),
                                              static_cast<int>(out.size()));
            if (n < 0 || static_cast<std::size_t>(n) != out.size()) {
                RIME_ERROR("FrameDecoder: LZ4 decode failed (returned {}, expected {} bytes)",
                           n,
                           out.size());
                return false;
            }
            return true;
        }

        case Codec::Jpeg: {
            if (jpeg_ == nullptr) {
                jpeg_ = tjInitDecompress();
                if (jpeg_ == nullptr) {
                    RIME_ERROR("FrameDecoder: tjInitDecompress failed: {}", tjGetErrorStr());
                    return false;
                }
            }
            const auto handle = static_cast<tjhandle>(jpeg_);
            const auto* src = reinterpret_cast<const unsigned char*>(in.data());
            const auto src_size = static_cast<unsigned long>(in.size());

            // Read the header first and check the stream's own dimensions against what the protocol
            // told us to expect. tjDecompress2 writes width*height*4 bytes into `out`; if a rogue
            // stream claimed a bigger size we would overflow — so validate before decoding.
            int w = 0;
            int h = 0;
            int subsamp = 0;
            int colorspace = 0;
            if (tjDecompressHeader3(handle, src, src_size, &w, &h, &subsamp, &colorspace) != 0) {
                RIME_ERROR("FrameDecoder: tjDecompressHeader3 failed: {}", tjGetErrorStr2(handle));
                return false;
            }
            if (w != static_cast<int>(desc.extent.width) ||
                h != static_cast<int>(desc.extent.height)) {
                RIME_ERROR("FrameDecoder: JPEG is {}x{}, expected {}x{}",
                           w,
                           h,
                           desc.extent.width,
                           desc.extent.height);
                return false;
            }
            const int pf = tj_pixel_format(desc.format);
            if (tjDecompress2(handle,
                              src,
                              src_size,
                              reinterpret_cast<unsigned char*>(out.data()),
                              w,
                              kTightlyPacked,
                              h,
                              pf,
                              0) != 0) {
                RIME_ERROR("FrameDecoder: tjDecompress2 failed: {}", tjGetErrorStr2(handle));
                return false;
            }
            return true;
        }
    }

    RIME_ERROR("FrameDecoder: unknown codec {}", static_cast<int>(codec));
    return false;
}

FrameDecoder::~FrameDecoder() {
    if (jpeg_ != nullptr) {
        tjDestroy(static_cast<tjhandle>(jpeg_));
    }
}

FrameDecoder::FrameDecoder(FrameDecoder&& other) noexcept : jpeg_(other.jpeg_) {
    other.jpeg_ = nullptr;
}

FrameDecoder& FrameDecoder::operator=(FrameDecoder&& other) noexcept {
    if (this != &other) {
        if (jpeg_ != nullptr) {
            tjDestroy(static_cast<tjhandle>(jpeg_));
        }
        jpeg_ = other.jpeg_;
        other.jpeg_ = nullptr;
    }
    return *this;
}

} // namespace rime::stream
