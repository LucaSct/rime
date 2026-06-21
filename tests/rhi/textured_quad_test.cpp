// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for brick M3.5 — and for M3's headline "done when": a *textured* quad, made deterministic.
// It renders the exact quad the 02-textured-quad sample renders (via the shared helper), reads the
// image back, and asserts each of the four quadrants carries its texel's color from the 2×2 R/G/B/Y
// test texture — top-left red, top-right green, bottom-left blue, bottom-right yellow — plus a corner
// outside the quad showing the dark clear color. Passing proves the whole M3.5 chain ran end to end:
// the index buffer drew the two triangles, the texture upload (staging copy) landed the right
// texels, the sampler read them, and the combined image-sampler descriptor reached the fragment
// shader — i.e. the shader really sampled the texture per pixel. Off-screen, so it runs on a software
// GPU (lavapipe) in CI with no display. (main() for this exe is in device_test.cpp.)

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "quad_render.hpp"

// The compiled quad shaders, embedded by rime_add_shaders (see tests/rhi/CMakeLists.txt).
#include "quad.frag.spv.h"
#include "quad.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi renders a textured quad off-screen (pixel-verified)") {
    auto device = rime::rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping offscreen render");
        return;
    }

    const std::uint32_t size = 32;
    const rime::rhi::ClearColor clear{0.1f, 0.1f, 0.3f, 1.0f}; // dark blue, shows at the corners
    const std::vector<std::uint8_t> px =
        rime_sample::render_quad_offscreen(*device,
                                           size,
                                           clear,
                                           quad_vert_spv,
                                           sizeof(quad_vert_spv),
                                           quad_frag_spv,
                                           sizeof(quad_frag_spv));
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    // RGBA8, row-major: pixel (x,y) starts at (y*size + x) * 4, channels R,G,B,A. The quad spans NDC
    // [-0.9, 0.9], leaving a ~2-pixel clear border around a ~28×28 quad. Vulkan's clip-space Y points
    // down, so uv (0,0) lands top-left: the quadrants read R,G / B,Y top-to-bottom (see make_quad).
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) -> const std::uint8_t* {
        return &px[(static_cast<std::size_t>(y) * size + x) * 4];
    };
    // Each texel is a pure color (every channel exactly 0 or 255) and we sample nearest, so on the
    // quad each channel reads ~0 or ~255: comparing to the 128 midpoint recovers which color landed.
    const auto bright_is = [&](std::uint32_t x, std::uint32_t y, bool r, bool g, bool b) {
        const std::uint8_t* p = pixel(x, y);
        return (p[0] > 128) == r && (p[1] > 128) == g && (p[2] > 128) == b;
    };

    // Sample each quadrant's center — a quarter and three-quarters along each axis — comfortably
    // inside one texel, away from both the clear border and the texel boundary at the image center.
    const std::uint32_t lo = size / 4;        // 8: center of the left / top quadrants
    const std::uint32_t hi = size - size / 4; // 24: center of the right / bottom quadrants

    CHECK(bright_is(lo, lo, true, false, false));  // top-left     -> red    (uv 0,0)
    CHECK(bright_is(hi, lo, false, true, false));  // top-right    -> green  (uv 1,0)
    CHECK(bright_is(lo, hi, false, false, true));  // bottom-left  -> blue   (uv 0,1)
    CHECK(bright_is(hi, hi, true, true, false));   // bottom-right -> yellow (uv 1,1)

    // A corner sits outside the quad -> the dark clear color: no channel is bright. (Its blue ~77
    // stays under the midpoint, so the corner is never mistaken for the blue texel.)
    CHECK(bright_is(0, 0, false, false, false));
}
