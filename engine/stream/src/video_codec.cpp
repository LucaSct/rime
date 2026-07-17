// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/stream/video_codec.hpp"

#include <dav1d/dav1d.h>
#include <EbSvtAv1Enc.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "rime/core/diagnostics/log.hpp"

// The video codec's implementation — SVT-AV1 in, dav1d out, confined to this translation unit (the
// public header speaks only std::byte and an opaque void*, the ADR-0017 discipline). Three parts:
//
//   1. **Colour conversion.** Video codecs do not speak RGBA: they work in Y'CbCr with the chroma
//      at quarter resolution (4:2:0), because the eye resolves brightness far more finely than
//      colour — half the samples for a loss you rarely see. So every frame is converted
//      RGBA -> planar I420 before encode and back after decode, with our own documented scalar
//      loops (the classic hidden cost of "just add a video codec"; codec_bench measures it).
//   2. **The encoder state machine.** SVT-AV1 is configured for interactive streaming:
//      low-delay prediction (no B-frames — nothing ever references the future, so nothing ever
//      waits for it), CBR rate control, keyframes only on demand. Its pipeline is asynchronous
//      (worker threads), so encode() polls the output queue until this frame's packet emerges —
//      one-in-one-out, the property the tests pin.
//   3. **The decoder loop.** dav1d's canonical send-data/get-picture loop, run to completion per
//      packet with max_frame_delay=1, so a fed frame comes straight back out.
namespace rime::stream {
namespace {

// ── RGBA <-> I420 colour conversion ─────────────────────────────────────────────────────────────
//
// Matrix: **full-range BT.601**, in 8.8 fixed point. Two deliberate choices:
//   - *Full range* (Y/Cb/Cr use all 0..255) rather than broadcast "studio swing" (16..235):
//     we control both ends of the wire, so squeezing 256 levels into 220 would only add
//     quantization loss. This matches JPEG/JFIF's YCbCr convention (the S0 codec's world), and we
//     declare it in the AV1 stream's colour description so any third-party decoder agrees.
//   - *Fixed point* (coefficients scaled by 256, +128 to round, >>8): exact enough that the
//     round-trip error is well under the codec's own loss, with no float math in the per-pixel
//     loop. C++20 defines >> on negative ints as arithmetic shift, so the rounding is portable.
//     The loops are plain scalar — clear first, SIMD only if codec_bench says the conversion
//     rivals the encode (it does not; encode dominates).

// Byte offsets of R,G,B within a 4-byte pixel for the formats we stream. RGBA/BGRA differ only in
// byte order; sRGB-ness does not change the byte layout (the codec is agnostic — the same values
// come back out, same as the S0 JPEG path).
struct ChannelOffsets {
    std::size_t r, g, b;
};

ChannelOffsets channel_offsets(rhi::Format format) {
    switch (format) {
        case rhi::Format::BGRA8Unorm:
        case rhi::Format::BGRA8Srgb:
            return {2, 1, 0};
        default: // RGBA8*; callers validated with is_supported_format()
            return {0, 1, 2};
    }
}

std::uint8_t clamp_u8(int v) {
    return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Interleaved RGBA/BGRA -> planar I420 (full 4:2:0: one Cb+Cr pair per 2x2 luma block, each the
// rounded average of the block's four chroma values — a box filter, the standard subsampling).
// Requires even width/height (enforced at open), which keeps the 2x2 walk branch-free.
void interleaved_to_i420(const std::byte* src,
                         std::uint32_t width,
                         std::uint32_t height,
                         ChannelOffsets off,
                         std::uint8_t* y_plane,
                         std::uint8_t* cb_plane,
                         std::uint8_t* cr_plane) {
    const std::uint32_t half_w = width / 2;
    for (std::uint32_t by = 0; by < height; by += 2) {
        for (std::uint32_t bx = 0; bx < width; bx += 2) {
            int cb_sum = 0;
            int cr_sum = 0;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint32_t x = bx + dx;
                    const std::uint32_t y = by + dy;
                    const std::byte* p = src + (static_cast<std::size_t>(y) * width + x) * 4u;
                    const int r = std::to_integer<int>(p[off.r]);
                    const int g = std::to_integer<int>(p[off.g]);
                    const int b = std::to_integer<int>(p[off.b]);
                    // Y = 0.299R + 0.587G + 0.114B — weights sum to 256/256, so Y stays in range.
                    y_plane[static_cast<std::size_t>(y) * width + x] =
                        static_cast<std::uint8_t>((77 * r + 150 * g + 29 * b + 128) >> 8);
                    cb_sum += ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                    cr_sum += ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                }
            }
            // Average the 2x2 block's chroma (+2 rounds). 0.5-coefficient overshoot (a fully
            // saturated channel maps to 256) is clamped here, once per block.
            cb_plane[static_cast<std::size_t>(by / 2) * half_w + bx / 2] =
                clamp_u8((cb_sum + 2) >> 2);
            cr_plane[static_cast<std::size_t>(by / 2) * half_w + bx / 2] =
                clamp_u8((cr_sum + 2) >> 2);
        }
    }
}

// Planar I420 -> interleaved RGBA/BGRA. Chroma is simply replicated across its 2x2 block (nearest
// neighbour) — bilinear chroma upsampling is a polish knob, invisible at streaming quality.
// Alpha is set opaque: a streamed viewport has no meaningful alpha, and video codecs do not carry
// one (the S0 JPEG path already had this property).
void i420_to_interleaved(const std::uint8_t* y_plane,
                         std::ptrdiff_t y_stride,
                         const std::uint8_t* cb_plane,
                         const std::uint8_t* cr_plane,
                         std::ptrdiff_t c_stride,
                         std::uint32_t width,
                         std::uint32_t height,
                         ChannelOffsets off,
                         std::byte* dst) {
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* yrow = y_plane + static_cast<std::ptrdiff_t>(y) * y_stride;
        const std::uint8_t* cbrow = cb_plane + static_cast<std::ptrdiff_t>(y / 2) * c_stride;
        const std::uint8_t* crrow = cr_plane + static_cast<std::ptrdiff_t>(y / 2) * c_stride;
        std::byte* drow = dst + static_cast<std::size_t>(y) * width * 4u;
        for (std::uint32_t x = 0; x < width; ++x) {
            const int luma = yrow[x];
            const int cb = cbrow[x / 2] - 128;
            const int cr = crrow[x / 2] - 128;
            std::byte* p = drow + static_cast<std::size_t>(x) * 4u;
            p[off.r] = static_cast<std::byte>(clamp_u8(luma + ((359 * cr + 128) >> 8)));
            p[off.g] = static_cast<std::byte>(clamp_u8(luma - ((88 * cb + 183 * cr + 128) >> 8)));
            p[off.b] = static_cast<std::byte>(clamp_u8(luma + ((454 * cb + 128) >> 8)));
            p[3] = static_cast<std::byte>(255);
        }
    }
}

// How long encode() will poll for the frame's packet before declaring the encoder wedged. The
// wait is normally the encode time itself (a frame at a real-time preset is milliseconds); the
// ceiling only exists so a library bug fails loudly instead of hanging a stream thread forever.
constexpr std::chrono::seconds kEncodePollTimeout{10};

} // namespace

// ── VideoEncoder ────────────────────────────────────────────────────────────────────────────────

// Everything SVT-AV1, boxed behind the header's void*. Heap-allocated once per open() and never
// moved, so the interior pointers below (io -> yuv buffer, input -> io) stay valid for its
// lifetime even when the owning VideoEncoder is std::move()d around.
struct VideoEncoderImpl {
    EbComponentType* handle = nullptr;
    EbSvtAv1EncConfiguration params{};
    // One reusable input picture: SVT copies the planes into its own ring during send_picture, so
    // a single conversion buffer serves the whole stream — no per-frame allocation.
    std::vector<std::uint8_t> yuv; // I420: w*h luma + 2 * (w/2)*(h/2) chroma
    EbSvtIOFormat io{};
    EbBufferHeaderType input{};
    bool initialized = false; // svt_av1_enc_init succeeded (deinit is only legal after it)
};

namespace {

void destroy_encoder_impl(VideoEncoderImpl* impl) {
    if (impl == nullptr) {
        return;
    }
    if (impl->handle != nullptr) {
        if (impl->initialized) {
            // Graceful shutdown: SVT-AV1 wants an **end-of-stream** picture before deinit (a flush
            // handshake — it logs an error otherwise). We run zero-delay, so nothing is buffered
            // and this only hands over the EOS marker and drains it straight back; but doing it
            // keeps teardown clean and silent. get_packet's third arg = 1 means "no more input"
            // (blocking flush), so the loop is bounded: SVT is guaranteed to emit the EOS packet we
            // wait for.
            EbBufferHeaderType eos{};
            eos.size = sizeof(EbBufferHeaderType);
            eos.flags = EB_BUFFERFLAG_EOS;
            eos.p_buffer = nullptr;
            if (svt_av1_enc_send_picture(impl->handle, &eos) == EB_ErrorNone) {
                for (;;) {
                    EbBufferHeaderType* pkt = nullptr;
                    const EbErrorType rc = svt_av1_enc_get_packet(impl->handle, &pkt, 1);
                    if (rc != EB_ErrorNone || pkt == nullptr) {
                        break;
                    }
                    const bool is_eos = (pkt->flags & EB_BUFFERFLAG_EOS) != 0;
                    svt_av1_enc_release_out_buffer(&pkt);
                    if (is_eos) {
                        break;
                    }
                }
            }
            svt_av1_enc_deinit(impl->handle);
        }
        svt_av1_enc_deinit_handle(impl->handle);
    }
    delete impl;
}

} // namespace

bool VideoEncoder::open(const Config& config) {
    close();

    const std::uint32_t w = config.desc.extent.width;
    const std::uint32_t h = config.desc.extent.height;
    if (!is_supported_format(config.desc.format)) {
        RIME_ERROR("VideoEncoder: unsupported format (video codec speaks 8-bit RGBA/BGRA only)");
        return false;
    }
    // Even dimensions: 4:2:0 stores one chroma sample per 2x2 luma block, so our subsampling walk
    // (and the wire format) wants whole blocks. >= 64: SVT-AV1's documented minimum picture size.
    if (w < 64 || h < 64 || (w % 2) != 0 || (h % 2) != 0) {
        RIME_ERROR(
            "VideoEncoder: {}x{} unsupported (v1 needs even dimensions, at least 64x64)", w, h);
        return false;
    }
    if (config.fps_num == 0 || config.fps_den == 0 || config.target_bitrate_kbps == 0) {
        RIME_ERROR("VideoEncoder: fps ({}/{}) and target bitrate ({} kbps) must be nonzero",
                   config.fps_num,
                   config.fps_den,
                   config.target_bitrate_kbps);
        return false;
    }
    if (config.preset < 0 || config.preset > 13) {
        RIME_ERROR("VideoEncoder: preset {} out of SVT-AV1's 0..13 range", config.preset);
        return false;
    }

    auto* impl = new VideoEncoderImpl();
    // STEP 1: create the handle; SVT fills `params` with library defaults we then override.
    if (svt_av1_enc_init_handle(&impl->handle, nullptr, &impl->params) != EB_ErrorNone) {
        RIME_ERROR("VideoEncoder: svt_av1_enc_init_handle failed");
        destroy_encoder_impl(impl);
        return false;
    }

    // STEP 2: configure for interactive streaming. Every choice here is latency-motivated:
    EbSvtAv1EncConfiguration& p = impl->params;
    p.source_width = w;
    p.source_height = h;
    p.frame_rate_numerator = config.fps_num;
    p.frame_rate_denominator = config.fps_den;
    p.encoder_bit_depth = 8;
    p.encoder_color_format = EB_YUV420;
    // enc_mode is the speed/compression dial (0 = slowest/best .. 13 = fastest); streaming rides
    // the fast end and codec_bench records what each notch costs.
    p.enc_mode = static_cast<std::int8_t>(config.preset);
    // Low-delay prediction: frames may only reference the *past*. The alternative (random access,
    // the default) uses B-frames that reference future frames — better compression, but the
    // encoder must buffer until that future arrives, which is built-in latency an interactive
    // stream cannot pay. This is also what makes encode() one-in-one-out.
    p.pred_structure = SVT_AV1_PRED_LOW_DELAY_B;
    // No lookahead for the same reason: lookahead trades delay for smarter rate decisions.
    p.look_ahead_distance = 0;
    // CBR at a fixed target: a steady, plannable wire rate (ADR-0030 §4 — fixed bitrate +
    // keyframe-on-request is the whole v1 rate story; adaptive control is S2).
    p.rate_control_mode = SVT_AV1_RC_MODE_CBR;
    p.target_bit_rate = config.target_bitrate_kbps * 1000u; // SVT wants bits/second
    // GOP policy: -1 = never insert periodic keyframes; intra only on the first frame and when the
    // app forces one per-picture (below, via pic_type on the input buffer — which SVT honours
    // natively in low-delay mode, so the separate force_key_frames flag is neither needed nor set).
    // Periodic keyframes are for *seekable broadcast*; on an ordered, reliable transport they are
    // pure bandwidth waste — a (re)joining client asks via the protocol's KeyframeRequest instead.
    p.intra_period_length =
        config.keyframe_interval == 0 ? -1 : static_cast<std::int32_t>(config.keyframe_interval);
    // Declare the colour convention our converter uses (full-range BT.601, see above) in the
    // bitstream, so any standards-following decoder reconstructs the same RGB we encoded.
    p.color_range = EB_CR_FULL_RANGE;
    p.matrix_coefficients = EB_CICP_MC_BT_601;
    if (config.parallelism != 0) {
        p.logical_processors = config.parallelism;
    }
    if (svt_av1_enc_set_parameter(impl->handle, &p) != EB_ErrorNone) {
        RIME_ERROR("VideoEncoder: svt_av1_enc_set_parameter rejected the configuration");
        destroy_encoder_impl(impl);
        return false;
    }

    // STEP 3: allocate the encoder (this also spawns SVT's internal worker threads).
    if (svt_av1_enc_init(impl->handle) != EB_ErrorNone) {
        RIME_ERROR("VideoEncoder: svt_av1_enc_init failed");
        destroy_encoder_impl(impl);
        return false;
    }
    impl->initialized = true;

    // Capture the sequence header now, out-of-band, so the protocol can hand it to decoders
    // before frame 0 (the StreamConfig message). The library owns the returned buffer; we copy
    // and release immediately.
    EbBufferHeaderType* header = nullptr;
    if (svt_av1_enc_stream_header(impl->handle, &header) != EB_ErrorNone || header == nullptr) {
        RIME_ERROR("VideoEncoder: svt_av1_enc_stream_header failed");
        destroy_encoder_impl(impl);
        return false;
    }
    const auto* header_bytes = reinterpret_cast<const std::byte*>(header->p_buffer);
    sequence_header_.assign(header_bytes, header_bytes + header->n_filled_len);
    svt_av1_enc_stream_header_release(header);

    // Wire the reusable input picture: planes point into our one conversion buffer.
    const std::size_t luma_size = static_cast<std::size_t>(w) * h;
    impl->yuv.resize(luma_size + luma_size / 2);
    impl->io.luma = impl->yuv.data();
    impl->io.cb = impl->yuv.data() + luma_size;
    impl->io.cr = impl->yuv.data() + luma_size + luma_size / 4;
    impl->io.y_stride = w;
    impl->io.cb_stride = w / 2;
    impl->io.cr_stride = w / 2;
    impl->io.width = w;
    impl->io.height = h;
    impl->input.size = sizeof(EbBufferHeaderType);
    impl->input.p_buffer = reinterpret_cast<std::uint8_t*>(&impl->io);
    impl->input.n_alloc_len = static_cast<std::uint32_t>(impl->yuv.size());
    impl->input.n_filled_len = static_cast<std::uint32_t>(impl->yuv.size());

    config_ = config;
    force_keyframe_ = false;
    next_pts_ = 0;
    impl_ = impl;
    return true;
}

void VideoEncoder::close() {
    destroy_encoder_impl(static_cast<VideoEncoderImpl*>(impl_));
    impl_ = nullptr;
    sequence_header_.clear();
    force_keyframe_ = false;
    next_pts_ = 0;
}

bool VideoEncoder::encode(std::span<const std::byte> pixels, std::vector<VideoPacket>& out) {
    out.clear();
    if (impl_ == nullptr) {
        RIME_ERROR("VideoEncoder: encode() before open()");
        return false;
    }
    if (pixels.size() != config_.desc.byte_size()) {
        RIME_ERROR("VideoEncoder: got {} pixel bytes, expected {} for {}x{}",
                   pixels.size(),
                   config_.desc.byte_size(),
                   config_.desc.extent.width,
                   config_.desc.extent.height);
        return false;
    }
    auto* impl = static_cast<VideoEncoderImpl*>(impl_);

    interleaved_to_i420(pixels.data(),
                        config_.desc.extent.width,
                        config_.desc.extent.height,
                        channel_offsets(config_.desc.format),
                        impl->io.luma,
                        impl->io.cb,
                        impl->io.cr);

    impl->input.pts = static_cast<std::int64_t>(next_pts_++);
    // EB_AV1_INVALID_PICTURE means "encoder's choice" (a delta frame mid-stream); KEY_PICTURE
    // forces the fresh chain the protocol's KeyframeRequest asked for.
    impl->input.pic_type = force_keyframe_ ? EB_AV1_KEY_PICTURE : EB_AV1_INVALID_PICTURE;
    impl->input.flags = 0;
    force_keyframe_ = false;
    if (svt_av1_enc_send_picture(impl->handle, &impl->input) != EB_ErrorNone) {
        RIME_ERROR("VideoEncoder: svt_av1_enc_send_picture failed");
        return false;
    }

    // SVT's pipeline is asynchronous — worker threads carry the picture through its stages, and
    // the only wait primitive is polling the output queue. Low-delay + no-lookahead guarantees
    // this frame's packet needs no future input, so polling until it emerges cannot deadlock; the
    // wait *is* the encode time. We then keep draining without sleeping in case a frame ever
    // yields more than one packet (it does not in low-delay, but the wire contract allows it).
    const auto deadline = std::chrono::steady_clock::now() + kEncodePollTimeout;
    bool got_packet = false;
    for (;;) {
        EbBufferHeaderType* packet = nullptr;
        const EbErrorType rc = svt_av1_enc_get_packet(impl->handle, &packet, 0);
        if (rc == EB_NoErrorEmptyQueue) {
            if (got_packet) {
                break; // this frame's packet(s) are all out
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                RIME_ERROR("VideoEncoder: no packet within {}s — encoder wedged?",
                           kEncodePollTimeout.count());
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        if (rc != EB_ErrorNone || packet == nullptr) {
            RIME_ERROR("VideoEncoder: svt_av1_enc_get_packet failed ({})",
                       static_cast<std::int32_t>(rc));
            return false;
        }
        VideoPacket& vp = out.emplace_back();
        const auto* data = reinterpret_cast<const std::byte*>(packet->p_buffer);
        vp.data.assign(data, data + packet->n_filled_len);
        vp.pts = static_cast<std::uint64_t>(packet->pts);
        vp.keyframe = packet->pic_type == EB_AV1_KEY_PICTURE;
        svt_av1_enc_release_out_buffer(&packet);
        got_packet = true;
    }
    return true;
}

VideoEncoder::~VideoEncoder() {
    close();
}

VideoEncoder::VideoEncoder(VideoEncoder&& other) noexcept
    : config_(other.config_), sequence_header_(std::move(other.sequence_header_)),
      force_keyframe_(other.force_keyframe_), next_pts_(other.next_pts_), impl_(other.impl_) {
    other.impl_ = nullptr;
    other.sequence_header_.clear();
}

VideoEncoder& VideoEncoder::operator=(VideoEncoder&& other) noexcept {
    if (this != &other) {
        close();
        config_ = other.config_;
        sequence_header_ = std::move(other.sequence_header_);
        force_keyframe_ = other.force_keyframe_;
        next_pts_ = other.next_pts_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
        other.sequence_header_.clear();
    }
    return *this;
}

// ── VideoDecoder ────────────────────────────────────────────────────────────────────────────────

struct VideoDecoderImpl {
    Dav1dContext* ctx = nullptr;
};

namespace {

void destroy_decoder_impl(VideoDecoderImpl* impl) {
    if (impl == nullptr) {
        return;
    }
    if (impl->ctx != nullptr) {
        dav1d_close(&impl->ctx); // nulls the pointer
    }
    delete impl;
}

// Copy `bytes` into a dav1d-owned buffer. dav1d reference-counts its input and may hold it past
// our call; *copying* into a buffer it owns makes the lifetime its problem entirely, instead of
// wrapping caller memory whose lifetime we would then have to guarantee. One memcpy of a
// few-kilobyte packet is noise next to the decode itself — ownership clarity wins.
bool make_dav1d_data(Dav1dData* data, std::span<const std::byte> bytes) {
    std::uint8_t* buf = dav1d_data_create(data, bytes.size());
    if (buf == nullptr) {
        return false;
    }
    std::memcpy(buf, bytes.data(), bytes.size());
    return true;
}

// Feed one packet through dav1d's canonical send/get loop (see dav1d.h), collecting the newest
// completed picture into `pic`. Send and receive interleave because dav1d may refuse to accept
// more data until a finished picture is taken off its output. Returns false on a decode error
// (`pic` is then unreferenced); `*have_pic` says whether a picture came out at all.
bool run_dav1d(Dav1dContext* ctx, Dav1dData* data, Dav1dPicture* pic, bool* have_pic) {
    *have_pic = false;
    auto take = [&](Dav1dPicture* fresh) {
        // Latest-wins, like the s1.1 readback ring: if one packet somehow completes several
        // pictures, a live viewport wants the newest, not a backlog.
        if (*have_pic) {
            dav1d_picture_unref(pic);
        }
        *pic = *fresh;
        *have_pic = true;
    };

    do {
        int rc = dav1d_send_data(ctx, data);
        if (rc < 0 && rc != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(data);
            RIME_ERROR("VideoDecoder: dav1d_send_data failed ({})", rc);
            return false;
        }
        Dav1dPicture fresh{};
        rc = dav1d_get_picture(ctx, &fresh);
        if (rc == 0) {
            take(&fresh);
        } else if (rc != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(data);
            if (*have_pic) {
                dav1d_picture_unref(pic);
                *have_pic = false;
            }
            RIME_ERROR("VideoDecoder: dav1d_get_picture failed ({})", rc);
            return false;
        }
    } while (data->sz > 0);

    // The packet is consumed; drain any picture that completed on the final send.
    for (;;) {
        Dav1dPicture fresh{};
        const int rc = dav1d_get_picture(ctx, &fresh);
        if (rc == 0) {
            take(&fresh);
            continue;
        }
        if (rc == DAV1D_ERR(EAGAIN)) {
            return true;
        }
        if (*have_pic) {
            dav1d_picture_unref(pic);
            *have_pic = false;
        }
        RIME_ERROR("VideoDecoder: dav1d_get_picture failed ({})", rc);
        return false;
    }
}

} // namespace

bool VideoDecoder::open(const Config& config, std::span<const std::byte> codec_config) {
    close();

    if (!is_supported_format(config.desc.format)) {
        RIME_ERROR("VideoDecoder: unsupported format (video codec speaks 8-bit RGBA/BGRA only)");
        return false;
    }
    if (config.desc.extent.width == 0 || config.desc.extent.height == 0) {
        RIME_ERROR("VideoDecoder: zero extent ({}x{})",
                   config.desc.extent.width,
                   config.desc.extent.height);
        return false;
    }
    if (codec_config.empty()) {
        RIME_ERROR("VideoDecoder: empty codec config (need the encoder's sequence header)");
        return false;
    }

    // Trust nothing: parse the out-of-band config *as* a sequence header and cross-check its
    // geometry against what the protocol announced, before standing anything up. A rogue or
    // mismatched StreamConfig fails here, loudly, instead of corrupting mid-stream.
    Dav1dSequenceHeader seq{};
    if (dav1d_parse_sequence_header(&seq,
                                    reinterpret_cast<const std::uint8_t*>(codec_config.data()),
                                    codec_config.size()) != 0) {
        RIME_ERROR("VideoDecoder: codec config is not a valid AV1 sequence header");
        return false;
    }
    if (seq.max_width != static_cast<int>(config.desc.extent.width) ||
        seq.max_height != static_cast<int>(config.desc.extent.height)) {
        RIME_ERROR("VideoDecoder: sequence header says {}x{}, StreamConfig says {}x{}",
                   seq.max_width,
                   seq.max_height,
                   config.desc.extent.width,
                   config.desc.extent.height);
        return false;
    }

    auto* impl = new VideoDecoderImpl();
    Dav1dSettings settings{};
    dav1d_default_settings(&settings);
    // max_frame_delay=1 is dav1d's low-latency mode: a decoded frame is handed out immediately
    // instead of being buffered for frame-parallel throughput. Same trade as the encoder side —
    // interactive streaming buys latency with throughput, never the reverse. n_threads stays 0
    // (auto): dav1d still tile-/task-parallelises within the frame.
    settings.max_frame_delay = 1;
    if (dav1d_open(&impl->ctx, &settings) != 0) {
        RIME_ERROR("VideoDecoder: dav1d_open failed");
        destroy_decoder_impl(impl);
        return false;
    }

    // Prime the decoder with the sequence header so the first wire packet can be a bare frame.
    // (Keyframe packets may repeat the header in-band; feeding it twice is legal AV1 — repeated
    // sequence headers are how broadcast streams let late tuners join.)
    Dav1dData data{};
    if (!make_dav1d_data(&data, codec_config)) {
        RIME_ERROR("VideoDecoder: failed to allocate codec-config buffer");
        destroy_decoder_impl(impl);
        return false;
    }
    Dav1dPicture pic{};
    bool have_pic = false;
    if (!run_dav1d(impl->ctx, &data, &pic, &have_pic)) {
        destroy_decoder_impl(impl);
        return false;
    }
    if (have_pic) {
        dav1d_picture_unref(&pic); // a header alone should not produce one; drop it if it did
    }

    config_ = config;
    impl_ = impl;
    return true;
}

void VideoDecoder::close() {
    destroy_decoder_impl(static_cast<VideoDecoderImpl*>(impl_));
    impl_ = nullptr;
}

bool VideoDecoder::decode(std::span<const std::byte> packet,
                          std::vector<std::byte>& out_pixels,
                          bool& frame_ready) {
    frame_ready = false;
    if (impl_ == nullptr) {
        RIME_ERROR("VideoDecoder: decode() before open()");
        return false;
    }
    if (packet.empty()) {
        RIME_ERROR("VideoDecoder: empty packet");
        return false;
    }
    auto* impl = static_cast<VideoDecoderImpl*>(impl_);

    Dav1dData data{};
    if (!make_dav1d_data(&data, packet)) {
        RIME_ERROR("VideoDecoder: failed to allocate packet buffer");
        return false;
    }
    Dav1dPicture pic{};
    bool have_pic = false;
    if (!run_dav1d(impl->ctx, &data, &pic, &have_pic)) {
        return false; // decoder survives: the next keyframe starts a fresh chain
    }
    if (!have_pic) {
        return true; // legal: a packet may complete no frame (e.g. parameter sets)
    }

    // Validate the *stream's* claim about geometry/format against our config before writing a
    // single pixel — the FrameDecoder JPEG-header check, replayed for video.
    const bool shape_ok = pic.p.w == static_cast<int>(config_.desc.extent.width) &&
                          pic.p.h == static_cast<int>(config_.desc.extent.height) &&
                          pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 && pic.p.bpc == 8;
    if (!shape_ok) {
        RIME_ERROR("VideoDecoder: picture is {}x{} layout {} bpc {}, expected {}x{} I420 8-bit",
                   pic.p.w,
                   pic.p.h,
                   static_cast<int>(pic.p.layout),
                   pic.p.bpc,
                   config_.desc.extent.width,
                   config_.desc.extent.height);
        dav1d_picture_unref(&pic);
        return false;
    }

    out_pixels.resize(config_.desc.byte_size());
    i420_to_interleaved(static_cast<const std::uint8_t*>(pic.data[0]),
                        pic.stride[0],
                        static_cast<const std::uint8_t*>(pic.data[1]),
                        static_cast<const std::uint8_t*>(pic.data[2]),
                        pic.stride[1],
                        config_.desc.extent.width,
                        config_.desc.extent.height,
                        channel_offsets(config_.desc.format),
                        out_pixels.data());
    dav1d_picture_unref(&pic);
    frame_ready = true;
    return true;
}

VideoDecoder::~VideoDecoder() {
    close();
}

VideoDecoder::VideoDecoder(VideoDecoder&& other) noexcept
    : config_(other.config_), impl_(other.impl_) {
    other.impl_ = nullptr;
}

VideoDecoder& VideoDecoder::operator=(VideoDecoder&& other) noexcept {
    if (this != &other) {
        close();
        config_ = other.config_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

} // namespace rime::stream
