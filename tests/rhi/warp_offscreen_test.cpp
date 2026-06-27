// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the viewer's vector-field warp (C3). A unit cube is warped by a vec3 field that displaces
// along +x proportionally to z (zero on the bottom slice, full on the top), rendered twice through the
// same render_warp_offscreen path the app uses: once with the warp gain and once with gain 0.
// Assertions: (1) the warp moves the surface (many pixels differ between the warped and unwarped
// images — vertex texture fetch + displacement worked), and (2) the surface is coloured by the field
// magnitude (cold-blue bottom + hot-red top), with no collapsed-normal black holes. Off-screen +
// readback, GPU-free on lavapipe in CI.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"

#include "camera.hpp"
#include "mesh_render.hpp"
#include "stl.hpp"
#include "warp.hpp"

#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "warp.frag.spv.h"
#include "warp.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("viewer warps the surface by a vector field (C3)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping warp render");
        return;
    }

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    const rime::viewer::CpuMesh cube = rime::viewer::make_unit_cube(); // x,y,z ∈ [-1, 1]
    const float r = cube.radius();

    // vec3 field: displace along +x, magnitude 0 on the bottom slice (z=-1) → 0.3 on the top (z=+1).
    const float vmag_max = 0.3f;
    std::vector<float> vol(2 * 2 * 2 * 4, 0.0f);
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const std::size_t gi = static_cast<std::size_t>(i) + 2 * (j + 2 * k);
                vol[gi * 4 + 0] = (k == 1) ? 0.3f : 0.0f; // displacement.x
                vol[gi * 4 + 3] = 1.0f;                    // validity
            }

    rime::viewer::OrbitCamera cam;
    cam.frame(cube.center(), cube.radius(), 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    const auto push_with_gain = [&](float gain) {
        rime::viewer::MeshPush p{};
        p.mvp = cam.view_proj(1.0f);
        const rime::core::Vec3 e = cam.eye();
        p.cam_pos[0] = e.x;
        p.cam_pos[1] = e.y;
        p.cam_pos[2] = e.z;
        p.cam_pos[3] = 1.0f;
        p.clip_plane[0] = p.clip_plane[1] = p.clip_plane[2] = 0.0f;
        p.clip_plane[3] = 1e30f;
        for (int c = 0; c < 3; ++c) {
            p.field_scale[c] = 0.25f; // cube [-1,1], 2-node axis → uvw = p*0.25 + 0.5
            p.field_bias[c] = 0.5f;
        }
        p.field_scale[3] = gain;
        p.field_bias[3] = vmag_max;
        return p;
    };

    const auto render = [&](float gain) {
        return rime::viewer::render_warp_offscreen(
            *device, size, cube, push_with_gain(gain), clear, mesh_vert_spv, sizeof(mesh_vert_spv),
            mesh_frag_spv, sizeof(mesh_frag_spv), warp_vert_spv, sizeof(warp_vert_spv), warp_frag_spv,
            sizeof(warp_frag_spv), vol.data(), 2, 2, 2);
    };
    const std::vector<std::uint8_t> warped = render(0.6f * r / vmag_max); // peak warp ~0.6·radius
    const std::vector<std::uint8_t> rest = render(0.0f);                  // undeformed
    REQUIRE(warped.size() == static_cast<std::size_t>(size) * size * 4);

    // (1) The warp moved the surface: many pixels differ between deformed and undeformed.
    std::size_t differ = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* a = &warped[static_cast<std::size_t>(i) * 4];
        const std::uint8_t* b = &rest[static_cast<std::size_t>(i) * 4];
        const int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
        if (dr * dr + dg * dg + db * db > 400) ++differ;
    }
    CHECK(differ > 100);

    // (2) Magnitude colormap on the warped surface: cold-blue (bottom) + hot-red (top), no black holes.
    std::size_t part = 0, blue = 0, red = 0, black = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &warped[static_cast<std::size_t>(i) * 4];
        if (p[0] < 8 && p[1] < 8 && p[2] < 8) ++black;
        const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        if (lum <= 40.0f) continue;
        ++part;
        if (p[2] > p[0] + 25 && p[2] > p[1]) ++blue;
        else if (p[0] > p[1] + 25 && p[0] > p[2] + 25) ++red;
    }
    CHECK(part > 0);
    CHECK(blue > 20);
    CHECK(red > 20);
    CHECK(black == 0);
}
