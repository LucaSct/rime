// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the viewer's streamlines (D·V). A synthetic vec3 velocity field flows along +z and speeds
// up downstream; build_streamlines integrates it (RK4) and render_streamlines_offscreen draws the lines
// coloured by speed. Assertions: the integrator produces lines (non-empty vertex list), they appear on
// screen, and the fast (downstream) end is hot-red — i.e. the lines are coloured by the computed speed.
// Off-screen + readback, GPU-free on lavapipe in CI.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"

#include "camera.hpp"
#include "field.hpp"
#include "mesh_render.hpp"
#include "streamlines.hpp"

#include "streamline.frag.spv.h"
#include "streamline.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("viewer traces and draws streamlines of a velocity field (D)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping streamline render");
        return;
    }

    // A vec3 velocity field on a cube [-1,1]^3, 8^3 grid: flow along +z, speeding up downstream
    // (v_z = 0.5 → 2.0 from bottom to top). validity 1 everywhere; vmag_max = 2.
    const std::uint32_t n = 8;
    rime::viewer::VectorField vf;
    vf.name = "velocity";
    vf.nx = vf.ny = vf.nz = n;
    vf.vmag_max = 2.0f;
    const float h = 2.0f / (n - 1); // cube spans [-1,1]
    for (int c = 0; c < 3; ++c) {
        vf.scale[c] = 1.0f / (h * n);
        vf.bias[c] = 0.5f / n - (-1.0f) / (h * n);
    }
    vf.rgba.assign(static_cast<std::size_t>(n) * n * n * 4, 0.0f);
    for (std::uint32_t k = 0; k < n; ++k)
        for (std::uint32_t j = 0; j < n; ++j)
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::size_t gi = (i + n * (j + static_cast<std::size_t>(n) * k)) * 4;
                vf.rgba[gi + 2] = 0.5f + 1.5f * static_cast<float>(k) / (n - 1); // v_z
                vf.rgba[gi + 3] = 1.0f;                                          // validity
            }

    // The integrator produces lines.
    const std::vector<float> verts = rime::viewer::build_streamlines(vf);
    CHECK(verts.size() > 32);     // several segments
    CHECK(verts.size() % 4 == 0); // vec4 vertices

    rime::viewer::OrbitCamera cam;
    cam.frame(rime::core::Vec3{0, 0, 0}, 1.732f, 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);
    rime::viewer::MeshPush push{};
    push.mvp = cam.view_proj(1.0f);

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    const std::vector<std::uint8_t> px = rime::viewer::render_streamlines_offscreen(
        *device, size, vf, push, clear, streamline_vert_spv, sizeof(streamline_vert_spv),
        streamline_frag_spv, sizeof(streamline_frag_spv));
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    std::size_t lines = 0, red = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        if (p[0] <= 40 && p[1] <= 40 && p[2] <= 40) continue; // background
        ++lines;
        if (p[0] > p[1] + 25 && p[0] > p[2] + 25) ++red; // hot (fast, downstream)
    }
    CHECK(lines > 50); // the streamlines are on screen
    CHECK(red > 5);    // ...and the fast end is coloured hot — speed colouring works
}
