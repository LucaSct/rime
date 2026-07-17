// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/rhi/types.hpp"

// The **codec** — the second brick of the graphics-streaming track (Track S / S0.3). The frame tap
// (S0.2) hands us a rendered frame's raw pixels; this shrinks them enough to cross a network before
// the protocol (S0.4) frames and sends them. Raw 1080p30 is ~250 MB/s — a non-starter on any real
// link — so streaming lives or dies on the codec.
//
// Two codecs, one interface, chosen by measurement (ADR-0017, docs/design/graphics-streaming.md):
//
//   - **Jpeg** — lossy DCT via libjpeg-turbo's SIMD-accelerated TurboJPEG API. ~15-30x smaller than
//     raw for a small, controllable quality loss: the only codec that fits a WAN budget, so it is
//     the S0 *wire* codec.
//   - **LZ4** — the fastest *lossless* byte compressor (multi-GB/s). Its ratio swings with content
//     (great on flat UI, ~1x on noisy 3-D), so it never beats JPEG for bandwidth — but it is exact
//     and nearly free, which is what a local/lossless path (the M9 editor viewport) will want.
//   - **Raw** — no compression, the 250 MB/s baseline. Kept as a uniform path for loopback and for
//     the benchmark's "what are we compressing against" reference.
//
// Both real codecs ship (games built on Rime stream through engine/stream); both libraries are
// permissive (libjpeg-turbo BSD-3/IJG, lz4 BSD-2) — Apache-2.0-safe per third_party/ policy. This
// header keeps them **opaque**: the library handles hide behind `void*`, so nothing that includes
// this pulls in <turbojpeg.h>/<lz4.h>. That is what lets the thin client (S0.5) decode with only
// rime::stream on its include path.
namespace rime::stream {

// Which codec produced (and must decode) a frame's bytes. These are **wire values** — the S0.4
// protocol header carries this as one byte — so they are fixed: append new codecs, never renumber.
//
// The division of labour after s1.2 (ADR-0030 §1): **Av1** is the inter-frame wire codec for
// bandwidth-constrained (WAN) links; **Jpeg** stays the stateless intra fallback every client can
// decode; **LZ4** stays the lossless/local path (the M9 editor viewport); **Raw** stays the
// baseline. Av1 frames are produced/consumed by the *stateful* VideoEncoder/VideoDecoder pair
// (video_codec.hpp), not by the stateless FrameEncoder/FrameDecoder below — inter-frame coding
// carries reference-picture state that a per-call API cannot.
enum class Codec : std::uint8_t {
    Raw = 0,  // no compression (memcpy) — the baseline
    LZ4 = 1,  // lossless, general-purpose
    Jpeg = 2, // lossy DCT — the intra-frame wire codec (S0), now the fallback
    Av1 = 3,  // lossy inter-frame video — the S1 wire codec (see video_codec.hpp)
};

// Just enough to describe a raw frame to encode / a decode target to fill. S0 speaks only 8-bit,
// 4-channel colour (RGBA8/BGRA8, un/sRGB) — what the offscreen target and both codecs handle — so
// every supported format is 4 bytes/pixel; byte_size() bakes that in.
struct ImageDesc {
    rhi::Extent2D extent{};
    rhi::Format format = rhi::Format::RGBA8Unorm;

    [[nodiscard]] std::size_t byte_size() const noexcept {
        return static_cast<std::size_t>(extent.width) * extent.height * 4u;
    }
};

// Is `format` one of the 8-bit 4-channel colour formats the S0 codecs accept?
[[nodiscard]] bool is_supported_format(rhi::Format format) noexcept;

// Encodes rendered frames. Stateful on purpose: it owns the (reusable) TurboJPEG compressor handle,
// so one encoder amortises libjpeg-turbo's setup across a whole stream — create it once, call
// encode() per frame. The handle is created lazily on the first Jpeg encode, so an encoder only
// ever used for Raw/LZ4 allocates nothing. Move-only (it owns the handle); not thread-safe
// (encode() mutates the handle) — use one encoder per stream, or one per worker.
class FrameEncoder {
public:
    struct Options {
        // JPEG quality 1..100 (libjpeg-turbo's scale). ~75-85 is the streaming sweet spot: near
        // visually-lossless on rendered content while staying ~20x smaller than raw.
        int jpeg_quality = 80;
    };

    FrameEncoder() = default;
    ~FrameEncoder();
    FrameEncoder(FrameEncoder&&) noexcept;
    FrameEncoder& operator=(FrameEncoder&&) noexcept;
    FrameEncoder(const FrameEncoder&) = delete;
    FrameEncoder& operator=(const FrameEncoder&) = delete;

    // Encode `pixels` (tightly packed, top row first, matching `desc`) with `codec` into `out`
    // (cleared, then resized to the encoded size). Returns false and logs — leaving `out` empty —
    // on an unsupported format, a pixels/desc size mismatch, or a library error.
    [[nodiscard]] bool encode(Codec codec,
                              const ImageDesc& desc,
                              std::span<const std::byte> pixels,
                              std::vector<std::byte>& out);

    void set_options(const Options& options) noexcept { options_ = options; }

    [[nodiscard]] const Options& options() const noexcept { return options_; }

private:
    Options options_{};
    void* jpeg_ = nullptr; // tjhandle (compress); lazily created, freed in the destructor
};

// Decodes frames produced by FrameEncoder. Owns the reusable TurboJPEG decompressor handle,
// mirroring the encoder. This is what the thin client (S0.5) runs to turn wire bytes back into
// pixels. Move-only, not thread-safe.
class FrameDecoder {
public:
    FrameDecoder() = default;
    ~FrameDecoder();
    FrameDecoder(FrameDecoder&&) noexcept;
    FrameDecoder& operator=(FrameDecoder&&) noexcept;
    FrameDecoder(const FrameDecoder&) = delete;
    FrameDecoder& operator=(const FrameDecoder&) = delete;

    // Decode `in` (produced by FrameEncoder with `codec`) into `out`, which must be exactly
    // desc.byte_size() bytes. For Jpeg the embedded dimensions are validated against `desc` before
    // writing, so a corrupt or rogue stream cannot make us write out of bounds. Returns false and
    // logs on a size mismatch, corrupt input, or a library error.
    [[nodiscard]] bool decode(Codec codec,
                              const ImageDesc& desc,
                              std::span<const std::byte> in,
                              std::span<std::byte> out);

private:
    void* jpeg_ = nullptr; // tjhandle (decompress); lazily created, freed in the destructor
};

} // namespace rime::stream
