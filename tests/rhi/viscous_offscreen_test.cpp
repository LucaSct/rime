// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for visualizing a *viscous* 3-D flow field (D2·V). What distinguishes ICEM's brick25
// Navier–Stokes field from the inviscid brick24 one is the no-slip **boundary layer**: the flow is
// fast in the core and falls to zero at the walls (a parabolic Poiseuille profile), not a uniform
// plug. This builds that synthetic field and checks the viewer shows the boundary layer three ways:
//   1. speed_field() derives the scalar speed |v| — a parabola, vmax at the centreline and 0 at the
//      no-slip walls (CPU-only, so this part runs even without a GPU).
//   2. streamlines coloured by speed show a hot-red fast core *and* cool-blue near-wall lines — the
//      cross-channel velocity gradient that viscosity creates.
//   3. the DVR of the derived speed scalar composites the fast core into a hot (red) cloud.
// Off-screen + readback, GPU-free on lavapipe in CI (the render parts skip if no device is
// present).

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "camera.hpp"
#include "field.hpp"
#include "iso.frag.spv.h"
#include "iso.hpp"
#include "iso.vert.spv.h"
#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "mesh_render.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"
#include "streamline.frag.spv.h"
#include "streamline.vert.spv.h"
#include "streamlines.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("viewer shows a viscous flow's boundary layer — speed field + streamlines + DVR (D2)") {
    using namespace rime::rhi;

    // A Poiseuille channel on the cube [-1,1]^3, n^3 nodes: the flow is along +z and its magnitude
    // is a parabola across x — v_z(x) = vmax·(1 − x²), so it is vmax on the centreline (x=0) and
    // exactly 0 at the no-slip walls (x=±1). n is odd so a node lands on the centreline. validity
    // is 1 throughout (the whole cube is fluid; no-slip is encoded by the velocity vanishing at the
    // walls, not by the mask).
    const std::uint32_t n = 13;
    const float vmax = 2.0f;
    const float h = 2.0f / (n - 1); // node spacing; the cube spans [-1, 1]
    rime::viewer::VectorField vf;
    vf.name = "velocity";
    vf.unit = "m/s";
    vf.nx = vf.ny = vf.nz = n;
    vf.vmag_max = vmax;
    for (int c = 0; c < 3; ++c) {
        vf.scale[c] = 1.0f / (h * n);
        vf.bias[c] = 0.5f / n - (-1.0f) / (h * n);
    }
    vf.rgba.assign(static_cast<std::size_t>(n) * n * n * 4, 0.0f);
    for (std::uint32_t k = 0; k < n; ++k)
        for (std::uint32_t j = 0; j < n; ++j)
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::size_t gi = (i + n * (j + static_cast<std::size_t>(n) * k)) * 4;
                const float x = -1.0f + static_cast<float>(i) * h; // world x of node i
                vf.rgba[gi + 2] = vmax * (1.0f - x * x); // v_z, parabolic across the channel
                vf.rgba[gi + 3] = 1.0f;                  // validity
            }

    // (1) The derived speed scalar is the parabola: vmax on the centreline, 0 at the walls, unit
    // carried.
    const rime::viewer::ScalarField sp = rime::viewer::speed_field(vf);
    const auto speed_at = [&](std::uint32_t i, std::uint32_t j, std::uint32_t kk) {
        return sp.rgba[(i + n * (j + static_cast<std::size_t>(n) * kk)) * 4];
    };
    CHECK(sp.nx == n);
    CHECK(sp.unit == "m/s");
    CHECK(sp.usable());
    CHECK(speed_at(n / 2, n / 2, n / 2) == doctest::Approx(vmax)); // centreline (x=0): fast
    CHECK(speed_at(0, n / 2, n / 2) == doctest::Approx(0.0f));     // no-slip wall (x=−1): stopped
    CHECK(sp.vmax == doctest::Approx(vmax));
    CHECK(sp.vmin == doctest::Approx(0.0f));

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the viscous-flow render");
        return;
    }

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    rime::viewer::OrbitCamera cam;
    cam.frame(rime::core::Vec3{0, 0, 0}, 1.732f, 1.0f); // cube [-1,1] → radius √3
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    // (2) Streamlines coloured by speed: the boundary layer shows as a hot core *and* cool
    // near-wall lines.
    rime::viewer::MeshPush push{};
    push.mvp = cam.view_proj(1.0f);
    const std::vector<std::uint8_t> spx =
        rime::viewer::render_streamlines_offscreen(*device,
                                                   size,
                                                   vf,
                                                   push,
                                                   clear,
                                                   streamline_vert_spv,
                                                   sizeof(streamline_vert_spv),
                                                   streamline_frag_spv,
                                                   sizeof(streamline_frag_spv));
    REQUIRE(spx.size() == static_cast<std::size_t>(size) * size * 4);

    std::size_t lines = 0, hot = 0, cool = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &spx[static_cast<std::size_t>(i) * 4];
        if (p[0] <= 40 && p[1] <= 40 && p[2] <= 40)
            continue; // background
        ++lines;
        if (p[0] > p[2] + 25)
            ++hot; // red > blue  → fast core
        if (p[2] > p[0] + 25)
            ++cool; // blue > red  → slow, near a no-slip wall
    }
    CHECK(lines > 50); // the streamlines are on screen
    CHECK(hot > 10);   // the fast core is hot-red
    CHECK(cool >
          10); // ...and the near-wall flow is cool-blue — the viscous boundary layer is visible

    // (3) DVR of the derived speed scalar: the fast core composites into a hot (red-dominated)
    // cloud, the
    //     slow walls staying near-transparent — the boundary layer rendered as a volume, not lines.
    const rime::viewer::CpuMesh cube = rime::viewer::make_unit_cube(); // owns the field volume only
    rime::viewer::IsoPush ip{};
    ip.inv_vp = rime::core::inverse(cam.view_proj(1.0f));
    for (int c = 0; c < 3; ++c) {
        ip.field_scale[c] = sp.scale[c];
        ip.field_bias[c] = sp.bias[c];
    }
    ip.field_scale[3] = 0.5f * (sp.vmin + sp.vmax); // isovalue (unused in DVR mode)
    ip.field_bias[3] = sp.vmin;
    ip.meta[0] = sp.vmax;
    ip.meta[1] = 192.0f; // ray steps
    ip.meta[2] = 1.0f;   // DVR
    const std::vector<std::uint8_t> dpx = rime::viewer::render_iso_offscreen(*device,
                                                                             size,
                                                                             cube,
                                                                             ip,
                                                                             clear,
                                                                             mesh_vert_spv,
                                                                             sizeof(mesh_vert_spv),
                                                                             mesh_frag_spv,
                                                                             sizeof(mesh_frag_spv),
                                                                             iso_vert_spv,
                                                                             sizeof(iso_vert_spv),
                                                                             iso_frag_spv,
                                                                             sizeof(iso_frag_spv),
                                                                             sp.rgba.data(),
                                                                             sp.nx,
                                                                             sp.ny,
                                                                             sp.nz);

    std::size_t cloud = 0;
    long rsum = 0, bsum = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &dpx[static_cast<std::size_t>(i) * 4];
        if (p[0] <= 40 && p[1] <= 40 && p[2] <= 40)
            continue; // background
        ++cloud;
        rsum += p[0];
        bsum += p[2];
    }
    CHECK(cloud > 50);  // the speed volume composites a visible cloud
    CHECK(rsum > bsum); // it is red-dominated — the hot fast core, not the cool walls
}
