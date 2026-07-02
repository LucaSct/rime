// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the viewer's raymarched isosurface / DVR (C2). A unit cube holds a scalar field that
// ramps along z (0 at the bottom → 1 at the top); render_iso_offscreen marches it for two isovalues
// and for a DVR. Assertions: each isosurface appears (sensible coverage); the low isovalue (z near
// the bottom) is cold-blue and the high one hot-red, so the isotherm sits where the colormap says
// it should; and the DVR composites the whole volume (more coverage than a single isosurface).
// Off-screen + readback, GPU-free on lavapipe in CI.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "camera.hpp"
#include "iso.frag.spv.h"
#include "iso.hpp"
#include "iso.vert.spv.h"
#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "mesh_render.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

struct Cover {
    std::size_t part = 0;
    long r = 0, g = 0, b = 0;
};
} // namespace

TEST_CASE("viewer raymarches an isosurface and a DVR of a scalar field (C2)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping isosurface render");
        return;
    }

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    const rime::viewer::CpuMesh cube = rime::viewer::make_unit_cube(); // x,y,z ∈ [-1, 1]

    // Scalar field ramping along z: value 0 on the bottom slice, 1 on the top; validity 1. 2×2×2.
    std::vector<float> vol(2 * 2 * 2 * 4, 0.0f);
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const std::size_t gi = static_cast<std::size_t>(i) + 2 * (j + 2 * k);
                vol[gi * 4 + 0] = (k == 0) ? 0.0f : 1.0f; // value
                vol[gi * 4 + 1] = 1.0f;                   // validity
            }

    rime::viewer::OrbitCamera cam;
    cam.frame(cube.center(), cube.radius(), 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    const auto make_push = [&](float iso, bool dvr) {
        rime::viewer::IsoPush p{};
        p.inv_vp = rime::core::inverse(cam.view_proj(1.0f));
        for (int c = 0; c < 3; ++c) {
            p.field_scale[c] = 0.25f; // cube [-1,1], 2-node axis → uvw = p*0.25 + 0.5
            p.field_bias[c] = 0.5f;
        }
        p.field_scale[3] = iso;
        p.field_bias[3] = 0.0f; // vmin
        p.meta[0] = 1.0f;       // vmax
        p.meta[1] = 192.0f;     // steps
        p.meta[2] = dvr ? 1.0f : 0.0f;
        return p;
    };
    const auto render = [&](float iso, bool dvr) {
        return rime::viewer::render_iso_offscreen(*device,
                                                  size,
                                                  cube,
                                                  make_push(iso, dvr),
                                                  clear,
                                                  mesh_vert_spv,
                                                  sizeof(mesh_vert_spv),
                                                  mesh_frag_spv,
                                                  sizeof(mesh_frag_spv),
                                                  iso_vert_spv,
                                                  sizeof(iso_vert_spv),
                                                  iso_frag_spv,
                                                  sizeof(iso_frag_spv),
                                                  vol.data(),
                                                  2,
                                                  2,
                                                  2);
    };
    const auto cover = [&](const std::vector<std::uint8_t>& px) {
        Cover c{};
        for (std::uint32_t i = 0; i < size * size; ++i) {
            const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
            if (p[0] <= 40 && p[1] <= 40 && p[2] <= 40)
                continue;
            ++c.part;
            c.r += p[0];
            c.g += p[1];
            c.b += p[2];
        }
        return c;
    };

    const Cover lo = cover(render(0.2f, false)); // cold end → blue/cyan
    const Cover hi = cover(render(0.8f, false)); // hot end → yellow/red
    const Cover dv = cover(render(0.5f, true));  // DVR

    // Both isosurfaces are on screen.
    CHECK(lo.part > 100);
    CHECK(hi.part > 100);
    // The low isovalue reads cold (blue dominant), the high one hot (red dominant): the isotherm
    // sits where the colormap places that value.
    CHECK(lo.b / static_cast<long>(lo.part) >
          lo.r / static_cast<long>(lo.part)); // blue > red (cold)
    CHECK(hi.r / static_cast<long>(hi.part) >
          hi.b / static_cast<long>(hi.part)); // red > blue (hot)
    // The DVR composites the whole volume → more coverage than a single thin isosurface.
    CHECK(dv.part > lo.part);
}
