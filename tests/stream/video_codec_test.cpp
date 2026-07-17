// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the s1.2 video codec (ADR-0030). Like the S0.3 codec proof this is **GPU-free** — the
// codec eats CPU pixel buffers — so it runs on every CI OS. What inter-frame coding promises, we
// assert:
//   - a *stream* of frames round-trips through VideoEncoder -> VideoDecoder at streaming quality
//     (PSNR-bounded — AV1 is lossy, so "close", never "equal"; determinism is explicitly not a
//     codec property);
//   - the reference implementation is **one-in-one-out** (a packet per frame, no hidden encoder
//     queue — queued frames are queued latency);
//   - a **forced keyframe** starts a chain a *fresh* decoder can join given only the out-of-band
//     sequence header — the protocol's join/recovery story, exercised end to end;
//   - malformed input is refused cleanly (trust nothing), and the decoder survives it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/rhi/types.hpp"
#include "rime/stream/video_codec.hpp"

using namespace rime;

namespace {

using Bytes = std::vector<std::byte>;

std::byte b(int v) {
    return static_cast<std::byte>(static_cast<std::uint8_t>(v & 0xFF));
}

int u8(std::byte x) {
    return static_cast<int>(std::to_integer<std::uint8_t>(x));
}

// The test content: a smooth gradient that *slides* with `t`. Smooth in space (keeps the lossy
// error small and the PSNR check un-flaky) but changing in time — so delta frames have real work
// to do, which is the property that separates a video codec from a JPEG-per-frame.
Bytes moving_gradient(std::uint32_t w, std::uint32_t h, int t) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::byte* p = &px[(static_cast<std::size_t>(y) * w + x) * 4u];
            p[0] = b(static_cast<int>((x * 255u) / w) + 2 * t);
            p[1] = b(static_cast<int>((y * 255u) / h));
            p[2] = b(static_cast<int>(((x + y) * 128u) / w) - t);
            p[3] = b(255);
        }
    }
    return px;
}

Bytes solid(std::uint32_t w, std::uint32_t h, int r, int g, int bl) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = b(r);
        px[i + 1] = b(g);
        px[i + 2] = b(bl);
        px[i + 3] = b(255);
    }
    return px;
}

// Peak signal-to-noise ratio (dB), the standard lossy-quality metric (higher is better; ~35 dB
// reads as visually clean on smooth content). Alpha is excluded: video carries no alpha channel —
// the decoder reconstitutes it as opaque — so comparing it would test a constant.
double psnr_rgb(const Bytes& a, const Bytes& c) {
    REQUIRE(a.size() == c.size());
    double mse = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        for (std::size_t ch = 0; ch < 3; ++ch) {
            const double e = static_cast<double>(u8(a[i + ch]) - u8(c[i + ch]));
            mse += e * e;
            ++n;
        }
    }
    mse /= static_cast<double>(n);
    if (mse <= 0.0) {
        return 1e9;
    }
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// A small, fast, real-time-ish configuration shared by the cases: preset 12 keeps the suite quick
// even in Debug; the bitrate is roomy for 320x180 so the PSNR floor has slack across platforms.
stream::VideoEncoder::Config test_config(rhi::Format format = rhi::Format::RGBA8Unorm) {
    stream::VideoEncoder::Config cfg;
    cfg.desc = {{320, 180}, format};
    cfg.fps_num = 30;
    cfg.fps_den = 1;
    cfg.target_bitrate_kbps = 1500;
    cfg.preset = 12;
    cfg.keyframe_interval = 0; // streaming policy: keyframes only at frame 0 + on request
    return cfg;
}

} // namespace

TEST_CASE("AV1 round-trips a moving stream within PSNR tolerance, one packet per frame") {
    const auto cfg = test_config();
    stream::VideoEncoder enc;
    REQUIRE(enc.open(cfg));
    // The out-of-band decoder bootstrap must exist the moment the encoder is open — the protocol
    // sends it (StreamConfig) before the first frame.
    REQUIRE(!enc.sequence_header().empty());

    stream::VideoDecoder dec;
    REQUIRE(dec.open({cfg.desc}, enc.sequence_header()));

    constexpr int kFrames = 40;
    std::size_t keyframe_bytes = 0;
    std::size_t delta_bytes = 0;
    std::vector<stream::VideoPacket> packets;
    Bytes decoded;
    for (int t = 0; t < kFrames; ++t) {
        const Bytes src = moving_gradient(320, 180, t);
        REQUIRE(enc.encode(src, packets));
        // One-in-one-out: the low-delay/no-lookahead configuration means no frame is ever held
        // back inside the encoder. Held-back frames would be invisible latency.
        REQUIRE(packets.size() == 1);
        CHECK(packets[0].pts == static_cast<std::uint64_t>(t));
        // Keyframe policy: exactly one, at stream start (interval 0 = never periodic).
        CHECK(packets[0].keyframe == (t == 0));
        (t == 0 ? keyframe_bytes : delta_bytes) += packets[0].data.size();

        bool ready = false;
        REQUIRE(dec.decode(packets[0].data, decoded, ready));
        REQUIRE(ready); // low-latency decode: the fed frame comes straight back out
        CHECK(psnr_rgb(src, decoded) > 30.0);
    }
    // The point of inter-frame coding: a delta frame of this slowly-changing content costs a
    // fraction of the full picture. (The quantified compression story is codec_bench's job; this
    // is the structural sanity check.)
    CHECK(delta_bytes / (kFrames - 1) < keyframe_bytes);
}

TEST_CASE("a forced keyframe starts a chain a fresh decoder can join") {
    const auto cfg = test_config();
    stream::VideoEncoder enc;
    REQUIRE(enc.open(cfg));

    std::vector<stream::VideoPacket> packets;
    std::vector<stream::VideoPacket> tail; // what a late joiner would receive
    for (int t = 0; t < 12; ++t) {
        if (t == 8) {
            enc.request_keyframe(); // the server half of the protocol's KeyframeRequest
        }
        const Bytes src = moving_gradient(320, 180, t);
        REQUIRE(enc.encode(src, packets));
        REQUIRE(packets.size() == 1);
        // Honored within one frame: the very next packet is the intra.
        CHECK(packets[0].keyframe == (t == 0 || t == 8));
        if (t >= 8) {
            tail.push_back(packets[0]);
        }
    }

    // A brand-new decoder, configured only out-of-band (the sequence header — what StreamConfig
    // carries), joins at the forced keyframe having never seen packets 0..7. This is exactly the
    // late-join/recovery flow ADR-0030 §4 builds on KeyframeRequest.
    stream::VideoDecoder joiner;
    REQUIRE(joiner.open({cfg.desc}, enc.sequence_header()));
    Bytes decoded;
    for (std::size_t i = 0; i < tail.size(); ++i) {
        bool ready = false;
        REQUIRE(joiner.decode(tail[i].data, decoded, ready));
        REQUIRE(ready);
        const Bytes src = moving_gradient(320, 180, 8 + static_cast<int>(i));
        CHECK(psnr_rgb(src, decoded) > 30.0);
    }
}

TEST_CASE("BGRA frames keep their channels through the YUV round-trip") {
    // Distinct per-channel values so a byte-order slip in the RGBA<->YUV conversion would show as
    // a swap (the same trap the S0 JPEG BGRA test guards). Solid colour decodes near-exactly.
    const auto cfg = test_config(rhi::Format::BGRA8Unorm);
    stream::VideoEncoder enc;
    REQUIRE(enc.open(cfg));
    stream::VideoDecoder dec;
    REQUIRE(dec.open({cfg.desc}, enc.sequence_header()));

    const Bytes src = solid(320, 180, 200, 120, 40); // B=200 G=120 R=40 in BGRA byte order
    std::vector<stream::VideoPacket> packets;
    REQUIRE(enc.encode(src, packets));
    REQUIRE(packets.size() == 1);
    Bytes out;
    bool ready = false;
    REQUIRE(dec.decode(packets[0].data, out, ready));
    REQUIRE(ready);
    CHECK(std::abs(u8(out[0]) - 200) < 8);
    CHECK(std::abs(u8(out[1]) - 120) < 8);
    CHECK(std::abs(u8(out[2]) - 40) < 8);
    CHECK(u8(out[3]) == 255); // video carries no alpha; it comes back opaque
}

TEST_CASE("encoder refuses invalid configs and calls") {
    stream::VideoEncoder enc;

    SUBCASE("odd dimensions are refused (4:2:0 wants whole 2x2 blocks)") {
        auto cfg = test_config();
        cfg.desc.extent = {321, 180};
        CHECK_FALSE(enc.open(cfg));
        CHECK_FALSE(enc.is_open());
    }
    SUBCASE("below SVT-AV1's 64x64 minimum is refused") {
        auto cfg = test_config();
        cfg.desc.extent = {32, 32};
        CHECK_FALSE(enc.open(cfg));
    }
    SUBCASE("unsupported pixel format is refused") {
        auto cfg = test_config();
        cfg.desc.format = rhi::Format::D32Float;
        CHECK_FALSE(enc.open(cfg));
    }
    SUBCASE("out-of-range preset is refused") {
        auto cfg = test_config();
        cfg.preset = 14;
        CHECK_FALSE(enc.open(cfg));
    }
    SUBCASE("encode before open is refused") {
        std::vector<stream::VideoPacket> packets;
        CHECK_FALSE(enc.encode(Bytes(320 * 180 * 4), packets));
    }
    SUBCASE("wrong-sized pixel span is refused") {
        REQUIRE(enc.open(test_config()));
        std::vector<stream::VideoPacket> packets;
        CHECK_FALSE(enc.encode(Bytes(320 * 180 * 4 - 4), packets));
    }
}

TEST_CASE("decoder trusts nothing") {
    const auto cfg = test_config();
    stream::VideoEncoder enc;
    REQUIRE(enc.open(cfg));

    SUBCASE("garbage codec config is refused at open") {
        stream::VideoDecoder dec;
        const Bytes garbage = {b(0xDE), b(0xAD), b(0xBE), b(0xEF), b(0x42)};
        CHECK_FALSE(dec.open({cfg.desc}, garbage));
        CHECK_FALSE(dec.is_open());
    }
    SUBCASE("a sequence header that contradicts the announced geometry is refused") {
        // A valid header — but from a different-sized stream. The decoder cross-checks instead of
        // believing whichever half of the config it saw first.
        stream::VideoEncoder other;
        auto other_cfg = test_config();
        other_cfg.desc.extent = {128, 64};
        REQUIRE(other.open(other_cfg));
        stream::VideoDecoder dec;
        CHECK_FALSE(dec.open({cfg.desc}, other.sequence_header()));
    }
    SUBCASE("empty codec config is refused") {
        stream::VideoDecoder dec;
        CHECK_FALSE(dec.open({cfg.desc}, {}));
    }
    SUBCASE("a corrupt packet fails cleanly and the decoder survives") {
        stream::VideoDecoder dec;
        REQUIRE(dec.open({cfg.desc}, enc.sequence_header()));

        Bytes garbage(64);
        for (std::size_t i = 0; i < garbage.size(); ++i) {
            garbage[i] = b(static_cast<int>(0xA5 ^ (i * 37)));
        }
        Bytes out;
        bool ready = true;
        CHECK_FALSE(dec.decode(garbage, out, ready));
        CHECK_FALSE(ready);

        // Recovery: the next *keyframe* starts a fresh chain (exactly what a real client does —
        // send KeyframeRequest, then resume at the intra that answers it).
        std::vector<stream::VideoPacket> packets;
        REQUIRE(enc.encode(moving_gradient(320, 180, 0), packets));
        REQUIRE(packets.size() == 1);
        REQUIRE(packets[0].keyframe);
        REQUIRE(dec.decode(packets[0].data, out, ready));
        CHECK(ready);
    }
    SUBCASE("empty packet is refused") {
        stream::VideoDecoder dec;
        REQUIRE(dec.open({cfg.desc}, enc.sequence_header()));
        Bytes out;
        bool ready = false;
        CHECK_FALSE(dec.decode({}, out, ready));
    }
    SUBCASE("decode before open is refused") {
        stream::VideoDecoder dec;
        Bytes out;
        bool ready = false;
        CHECK_FALSE(dec.decode(Bytes(16), out, ready));
    }
}
