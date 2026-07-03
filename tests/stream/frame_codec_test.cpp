// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the S0.3 codec. Unlike the frame-tap proof, this needs **no GPU**: the codec operates
// on CPU pixel buffers, so we synthesize frames in memory, run them through
// FrameEncoder/FrameDecoder, and check the result. That is deliberate — the plan wants codec
// coverage that runs GPU-free on every CI OS (even the ones with no Vulkan device). We assert what
// each codec promises:
//   - Raw / LZ4 are **lossless**: the decode is bit-identical to the input.
//   - JPEG is **lossy**: the decode is *close* (small mean error) and much smaller on the wire.
//   - both reject malformed input rather than reading/writing out of bounds.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/types.hpp"
#include "rime/stream/frame_codec.hpp"

using namespace rime;

namespace {

using Bytes = std::vector<std::byte>;

std::byte b(int v) {
    return static_cast<std::byte>(static_cast<std::uint8_t>(v));
}

int u8(std::byte x) {
    return static_cast<int>(std::to_integer<std::uint8_t>(x));
}

// A smooth RGBA gradient: cheap for both codecs (JPEG loves low frequencies; LZ4 finds some runs),
// and its smoothness keeps JPEG's error low so the lossy check is not flaky.
Bytes gradient(std::uint32_t w, std::uint32_t h) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::byte* p = &px[(static_cast<std::size_t>(y) * w + x) * 4u];
            p[0] = b(static_cast<int>(x * 255u / (w - 1u)));
            p[1] = b(static_cast<int>(y * 255u / (h - 1u)));
            p[2] = b(static_cast<int>((x + y) * 255u / (w + h - 2u)));
            p[3] = b(255);
        }
    }
    return px;
}

// A solid colour: the flat region LZ4 compresses to almost nothing and JPEG reproduces
// near-exactly.
Bytes solid(std::uint32_t w, std::uint32_t h, int r, int g, int bl, int a) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = b(r);
        px[i + 1] = b(g);
        px[i + 2] = b(bl);
        px[i + 3] = b(a);
    }
    return px;
}

// Deterministic "noise": the worst case for a lossless codec (incompressible — LZ4 may even grow it
// slightly). A tiny LCG keeps it reproducible across machines.
Bytes noise(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    std::uint32_t s = seed;
    for (std::size_t i = 0; i < px.size(); ++i) {
        s = s * 1664525u + 1013904223u; // Numerical Recipes LCG
        px[i] = b(static_cast<int>((s >> 24) & 0xFFu));
    }
    return px;
}

// Mean absolute per-byte difference — a simple, robust distortion metric for the lossy check.
double mean_abs_error(const Bytes& a, const Bytes& c) {
    REQUIRE(a.size() == c.size());
    long sum = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += std::abs(u8(a[i]) - u8(c[i]));
    }
    return static_cast<double>(sum) / static_cast<double>(a.size());
}

} // namespace

TEST_CASE("Raw codec is an exact passthrough") {
    const stream::ImageDesc desc{{40, 30}, rhi::Format::RGBA8Unorm};
    const Bytes src = gradient(40, 30);

    stream::FrameEncoder enc;
    Bytes wire;
    REQUIRE(enc.encode(stream::Codec::Raw, desc, src, wire));
    CHECK(wire.size() == desc.byte_size()); // no compression: same size

    stream::FrameDecoder dec;
    Bytes out(desc.byte_size());
    REQUIRE(dec.decode(stream::Codec::Raw, desc, wire, out));
    CHECK(out == src); // bit-exact
}

TEST_CASE("LZ4 is lossless across flat, smooth, and incompressible frames") {
    const stream::ImageDesc desc{{64, 64}, rhi::Format::RGBA8Unorm};
    stream::FrameEncoder enc;
    stream::FrameDecoder dec;

    SUBCASE("solid colour shrinks a lot and round-trips exactly") {
        const Bytes src = solid(64, 64, 30, 90, 200, 255);
        Bytes wire;
        REQUIRE(enc.encode(stream::Codec::LZ4, desc, src, wire));
        CHECK(wire.size() < desc.byte_size() / 4); // flat data is very compressible
        Bytes out(desc.byte_size());
        REQUIRE(dec.decode(stream::Codec::LZ4, desc, wire, out));
        CHECK(out == src);
    }
    SUBCASE("gradient round-trips exactly (lossless, unlike JPEG)") {
        const Bytes src = gradient(64, 64);
        Bytes wire;
        REQUIRE(enc.encode(stream::Codec::LZ4, desc, src, wire));
        Bytes out(desc.byte_size());
        REQUIRE(dec.decode(stream::Codec::LZ4, desc, wire, out));
        CHECK(out == src);
    }
    SUBCASE("incompressible noise still round-trips exactly") {
        const Bytes src = noise(64, 64, 0xC0FFEEu);
        Bytes wire;
        REQUIRE(enc.encode(stream::Codec::LZ4, desc, src, wire)); // may be ~= input size
        Bytes out(desc.byte_size());
        REQUIRE(dec.decode(stream::Codec::LZ4, desc, wire, out));
        CHECK(out == src);
    }
}

TEST_CASE("JPEG is small and visually close on a smooth frame") {
    const stream::ImageDesc desc{{128, 96}, rhi::Format::RGBA8Unorm};
    const Bytes src = gradient(128, 96);

    stream::FrameEncoder enc;
    Bytes wire;
    REQUIRE(enc.encode(stream::Codec::Jpeg, desc, src, wire));
    // The whole point of JPEG here: a big bandwidth win. A smooth 128x96 frame must land well under
    // a tenth of its raw size.
    CHECK(wire.size() < desc.byte_size() / 10);

    stream::FrameDecoder dec;
    Bytes out(desc.byte_size());
    REQUIRE(dec.decode(stream::Codec::Jpeg, desc, wire, out));
    CHECK(out.size() == desc.byte_size());
    // Lossy, but only a little: mean per-byte error on a smooth gradient at quality 80 is tiny.
    CHECK(mean_abs_error(src, out) < 8.0);
}

TEST_CASE("JPEG round-trips a BGRA frame with bytes in place") {
    const stream::ImageDesc desc{{32, 32}, rhi::Format::BGRA8Unorm};
    // Distinct per-channel values (B=200, G=120, R=40) so a byte-order slip would show as a swap.
    // We tell TurboJPEG this frame is BGRA on both ends, so the bytes must come back where they
    // left.
    const Bytes src = solid(32, 32, 200, 120, 40, 255);

    stream::FrameEncoder enc;
    stream::FrameDecoder dec;
    Bytes wire;
    REQUIRE(enc.encode(stream::Codec::Jpeg, desc, src, wire));
    Bytes out(desc.byte_size());
    REQUIRE(dec.decode(stream::Codec::Jpeg, desc, wire, out));
    CHECK(std::abs(u8(out[0]) - 200) < 6);
    CHECK(std::abs(u8(out[1]) - 120) < 6);
    CHECK(std::abs(u8(out[2]) - 40) < 6);
}

TEST_CASE("one encoder/decoder handles a stream of frames (handle reuse)") {
    const stream::ImageDesc desc{{48, 48}, rhi::Format::RGBA8Unorm};
    stream::FrameEncoder enc;
    stream::FrameDecoder dec;
    Bytes out(desc.byte_size());
    // The encoder lazily creates its TurboJPEG handle on the first Jpeg frame and reuses it after;
    // encoding several frames must all succeed on the one handle.
    for (int i = 0; i < 5; ++i) {
        const Bytes src = solid(48, 48, 10 * i, 255 - 5 * i, 100, 255);
        Bytes wire;
        REQUIRE(enc.encode(stream::Codec::Jpeg, desc, src, wire));
        REQUIRE(dec.decode(stream::Codec::Jpeg, desc, wire, out));
    }
}

TEST_CASE("codec rejects malformed calls instead of overrunning") {
    const stream::ImageDesc desc{{16, 16}, rhi::Format::RGBA8Unorm};
    stream::FrameEncoder enc;
    stream::FrameDecoder dec;

    SUBCASE("unsupported format is refused") {
        CHECK_FALSE(stream::is_supported_format(rhi::Format::D32Float));
        Bytes wire;
        const stream::ImageDesc bad{{16, 16}, rhi::Format::D32Float};
        CHECK_FALSE(enc.encode(stream::Codec::Jpeg, bad, Bytes(16 * 16 * 4), wire));
    }
    SUBCASE("wrong-sized pixel span is refused") {
        Bytes wire;
        CHECK_FALSE(enc.encode(stream::Codec::Raw, desc, Bytes(desc.byte_size() - 4), wire));
    }
    SUBCASE("wrong-sized decode target is refused") {
        Bytes wire;
        REQUIRE(enc.encode(stream::Codec::Raw, desc, solid(16, 16, 1, 2, 3, 4), wire));
        Bytes too_small(desc.byte_size() - 4);
        CHECK_FALSE(dec.decode(stream::Codec::Raw, desc, wire, too_small));
    }
    SUBCASE("empty input is refused") {
        Bytes out(desc.byte_size());
        CHECK_FALSE(dec.decode(stream::Codec::LZ4, desc, Bytes{}, out));
    }
    SUBCASE("corrupt LZ4 input is refused, not decoded past the buffer") {
        Bytes garbage = {b(0xFF), b(0x00), b(0x13), b(0x37), b(0x42), b(0x99)};
        Bytes out(desc.byte_size());
        CHECK_FALSE(dec.decode(stream::Codec::LZ4, desc, garbage, out));
    }
    SUBCASE("JPEG whose dimensions disagree with the header is refused") {
        // Encode a 16x16 frame, then try to decode it as if it were 8x8: the embedded size (16x16)
        // must be caught against the (smaller) expected desc before any pixels are written.
        Bytes wire;
        REQUIRE(enc.encode(stream::Codec::Jpeg, desc, gradient(16, 16), wire));
        const stream::ImageDesc smaller{{8, 8}, rhi::Format::RGBA8Unorm};
        Bytes out(smaller.byte_size());
        CHECK_FALSE(dec.decode(stream::Codec::Jpeg, smaller, wire, out));
    }
}
