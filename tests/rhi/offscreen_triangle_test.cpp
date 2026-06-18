// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for brick M3.3 — "first pixels", made deterministic. It renders the exact triangle the
// 01-hello-triangle sample renders (via the shared helper), reads the image back, and asserts:
//   - the center pixel is the triangle's solid red, and
//   - a corner pixel is the dark-blue clear color.
// Together those prove a real triangle was rasterized into the framebuffer — and because the render
// is off-screen, the whole thing runs on a software GPU (lavapipe) in CI, with no display or
// hardware GPU. (main() for this exe is in device_test.cpp.)

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "triangle_render.hpp"

// The compiled triangle shaders, embedded by rime_add_shaders (see tests/rhi/CMakeLists.txt).
#include "triangle.frag.spv.h"
#include "triangle.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi renders a triangle off-screen (pixel-verified)") {
    auto device = rime::rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping offscreen render");
        return;
    }

    const std::uint32_t size = 64;
    const rime::rhi::ClearColor clear{0.1f, 0.1f, 0.3f, 1.0f}; // dark blue
    const std::vector<std::uint8_t> px =
        rime_sample::render_triangle_offscreen(*device,
                                               size,
                                               clear,
                                               triangle_vert_spv,
                                               sizeof(triangle_vert_spv),
                                               triangle_frag_spv,
                                               sizeof(triangle_frag_spv));
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    // RGBA8, row-major: pixel (x,y) starts at (y*size + x) * 4, channels R,G,B,A.
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) -> const std::uint8_t* {
        return &px[(static_cast<std::size_t>(y) * size + x) * 4];
    };

    // Center is inside the triangle -> solid red (R high, G/B low).
    const std::uint8_t* center = pixel(size / 2, size / 2);
    CHECK(center[0] > 180); // R
    CHECK(center[1] < 80);  // G
    CHECK(center[2] < 80);  // B

    // A corner is outside the triangle -> the clear color (its red channel is ~25/255).
    const std::uint8_t* corner = pixel(1, 1);
    CHECK(corner[0] < 80); // not the red triangle
}
