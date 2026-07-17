// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/stream/frame_codec.hpp"

// The **video codec** — Track S / s1.2 (ADR-0030). The S0 codecs (frame_codec.hpp) are
// *intra-frame*: every frame is compressed alone, so a mostly-static viewport pays full-frame
// bandwidth 30x a second even when nothing moves. A real video codec is *inter-frame*: it sends a
// full picture only occasionally (a **keyframe**, decodable on its own) and, between keyframes,
// only what changed since the previous frame (**delta frames**, which reference earlier ones).
// That temporal compression is the single biggest bandwidth win left after S0 — an order of
// magnitude on typical viewport content.
//
// The codec is **AV1**, chosen on licensing (ADR-0030 §1): AV1 is royalty-free by design, while
// H.264/HEVC ride patent pools that would muddy shipping Rime under Apache-2.0 — the same trap
// that ruled out GPL x264 in ADR-0017. Software implementations, both permissive and both shipped:
//
//   - **SVT-AV1** encodes (BSD-3-Clause + AOM Patent License 1.0) — built for scalable multi-core
//     software encode, with the low-latency presets streaming needs.
//   - **dav1d** decodes (BSD-2-Clause) — small and fast; shipping it means every client can decode
//     AV1 without renting a patent-encumbered decoder.
//
// Inter-frame coding is inherently **stateful** — the encoder and decoder each carry the reference
// pictures deltas are measured against — so it cannot hide behind the stateless per-call
// FrameEncoder/FrameDecoder API. Instead: a VideoEncoder/VideoDecoder *pair per stream*, opened
// with the stream's parameters and fed frames in order. Losing or reordering packets breaks a
// delta chain until the next keyframe; the S0 transport is TCP (ordered, reliable), so v1 only
// needs keyframes for *joining* and *reset* — which is what the protocol's KeyframeRequest is for.
//
// These classes are **seams, not just wrappers** (ADR-0030 §3): the interface is what the engine
// depends on, and software SVT-AV1/dav1d is its reference implementation (and the CI/lavapipe
// path — provably GPU-free). Hardware encoders (VideoToolbox, VAAPI/QuickSync, NVENC) slot in
// per-platform behind this same shape later, with no interface or protocol change — the wire
// carries `Codec::Av1` regardless of which encoder produced the bits.
//
// Like frame_codec.hpp, this header keeps the libraries **opaque**: the SVT-AV1/dav1d state hides
// behind a `void*`, so nothing that includes this pulls in <svt-av1/...> or <dav1d/...>. The thin
// client decodes with only rime::stream on its include path; both libraries link PRIVATE.
namespace rime::stream {

// One encoder output: a compressed AV1 packet, the unit a FrameMessage carries. `pts` echoes the
// input-frame ordinal the packet encodes (presentation timestamp — trivially the frame index at a
// fixed rate; the s1.3 latency ledger keys off it). `keyframe` marks a packet that starts a fresh
// delta chain — what a joining/recovering client must wait for.
struct VideoPacket {
    std::vector<std::byte> data;
    std::uint64_t pts = 0;
    bool keyframe = false;
};

// Encodes an ordered stream of raw frames into AV1 packets. One per stream; move-only, not
// thread-safe (encode() mutates the codec state) — SVT-AV1 parallelises *internally* across its
// own worker threads, so a single encoder already uses the machine.
class VideoEncoder {
public:
    struct Config {
        // Frame geometry + pixel layout (RGBA8/BGRA8, un/sRGB — the same set the S0 codecs
        // accept, see is_supported_format). v1 constraints, checked at open(): dimensions must be
        // even (4:2:0 chroma is stored per 2x2 block) and at least 64x64 (SVT-AV1's minimum).
        ImageDesc desc{};

        // Frame rate as a rational (30/1 = 30 fps). The encoder uses it to translate
        // target_bitrate_kbps into per-frame bit budgets.
        std::uint32_t fps_num = 30;
        std::uint32_t fps_den = 1;

        // CBR target. Constant-bitrate + keyframe-on-request is the whole v1 rate story
        // (ADR-0030 §4): a *steady, predictable* wire rate a WAN link can plan around, unlike
        // per-frame JPEG whose size swings with content. Adaptive rate control is S2.
        std::uint32_t target_bitrate_kbps = 4000;

        // SVT-AV1 preset ("enc_mode"), 0 (slowest, best compression) .. 13 (fastest). Streaming
        // wants the low-latency end: 10-12 are the real-time presets on desktop CPUs; the
        // codec_bench numbers (ADR-0030 defers them to this brick) quantify the trade.
        int preset = 10;

        // Frames between forced keyframes. 0 = the streaming default: only the first frame and
        // explicit request_keyframe() calls produce keyframes. Periodic keyframes are a
        // *broadcast* idiom (random seeking); an interactive stream over TCP never loses packets,
        // so periodic intra bandwidth would be pure waste — a joiner asks instead
        // (KeyframeRequest).
        std::uint32_t keyframe_interval = 0;

        // Cap on SVT-AV1's internal worker parallelism. 0 = library default (use the machine).
        // Handy for benchmarking the single-core cost and for pinning the encoder off the
        // simulation's cores later.
        std::uint32_t parallelism = 0;
    };

    VideoEncoder() = default;
    ~VideoEncoder();
    VideoEncoder(VideoEncoder&&) noexcept;
    VideoEncoder& operator=(VideoEncoder&&) noexcept;
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Create the SVT-AV1 encoder for `config`. Two-phase init (open() rather than a throwing
    // constructor) because codec setup can fail for prosaic reasons — bad dimensions, an
    // out-of-range preset — and engine code reports errors by status, not exceptions.
    // Returns false (logged) on invalid config or a library error; the encoder is then not open.
    [[nodiscard]] bool open(const Config& config);

    // Release the codec state. Safe to call when not open; open() may be called again after.
    void close();

    [[nodiscard]] bool is_open() const noexcept { return impl_ != nullptr; }

    // The AV1 **sequence header** (the H.264 SPS/PPS analog): global stream parameters —
    // resolution, profile, bit depth — a decoder must know *before* the first frame. Captured
    // once at open(); the protocol ships it in the StreamConfig message so a client can configure
    // its decoder out-of-band (and a late joiner does not have to wait to scrape one off the
    // wire). Valid while the encoder is open; empty when closed.
    [[nodiscard]] std::span<const std::byte> sequence_header() const noexcept {
        return sequence_header_;
    }

    // Make the *next* encoded frame a keyframe. This is the server half of the protocol's
    // KeyframeRequest: a fresh delta chain on demand — on client join, after a decoder reset, and
    // (S2) after loss. Cheap flag; takes effect on the next encode() call.
    void request_keyframe() noexcept { force_keyframe_ = true; }

    // Encode one frame (tightly packed, top row first, matching config.desc — exactly the
    // FrameStreamer layout). Appends the resulting packet(s) to `out` (cleared first).
    //
    // The *interface* contract is 0..n packets per call: an encoder is allowed to buffer (a
    // hardware encoder's pipeline depth, or B-frame lookahead — which v1 never enables: a frame
    // that references the *future* cannot be encoded until that future arrives, i.e. built-in
    // latency, so interactive streaming forbids B-frames). This software reference is configured
    // zero-delay and is strictly **one-in-one-out**: every call yields exactly one packet — the
    // property the round-trip test pins, because encoder-side queueing is invisible latency.
    // Returns false (logged) on a size mismatch or a library error.
    [[nodiscard]] bool encode(std::span<const std::byte> pixels, std::vector<VideoPacket>& out);

    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    Config config_{};
    std::vector<std::byte> sequence_header_{};
    bool force_keyframe_ = false;
    std::uint64_t next_pts_ = 0;
    void* impl_ = nullptr; // SVT-AV1 handle + reusable YUV/IO buffers; freed in close()
};

// Decodes the packet stream a VideoEncoder produced back into raw frames. One per stream;
// move-only, not thread-safe. This is what the thin client runs — it needs only rime::stream.
class VideoDecoder {
public:
    struct Config {
        // The geometry/format the server announced (StreamConfig message). Decoded pictures are
        // validated against it: a stream whose embedded size disagrees is refused rather than
        // trusted — same trust-nothing stance as FrameDecoder's JPEG header check.
        ImageDesc desc{};
    };

    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Create the dav1d decoder and prime it with `codec_config` — the encoder's
    // sequence_header() bytes, carried to us by the StreamConfig message. The config is validated
    // (it must parse as an AV1 sequence header whose resolution matches `config.desc`) before any
    // packet is accepted, so a malformed or mismatched stream fails at open, loudly, not
    // mid-stream. Returns false (logged) on invalid config or a library error.
    [[nodiscard]] bool open(const Config& config, std::span<const std::byte> codec_config);

    void close();

    [[nodiscard]] bool is_open() const noexcept { return impl_ != nullptr; }

    // Feed one wire packet (one VideoPacket::data / FrameMessage payload). If it completes a
    // frame, the pixels are written into `out_pixels` (resized to desc.byte_size(), same tightly
    // packed layout the encoder consumed) and `frame_ready` is set. A packet may legitimately
    // complete no frame (e.g. stray parameter sets) — that is not an error; `frame_ready` stays
    // false. Should one packet carry several frames, the *newest* wins (latest-wins, the s1.1
    // drop policy: a viewport wants the freshest picture, not a backlog).
    //
    // Corrupt or truncated input is a clean `false` (logged) — dav1d validates the bitstream;
    // we never write past out_pixels. After an error the decoder stays usable: feed it the next
    // keyframe (request one!) to start a fresh delta chain.
    [[nodiscard]] bool decode(std::span<const std::byte> packet,
                              std::vector<std::byte>& out_pixels,
                              bool& frame_ready);

    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    Config config_{};
    void* impl_ = nullptr; // dav1d context; freed in close()
};

} // namespace rime::stream
