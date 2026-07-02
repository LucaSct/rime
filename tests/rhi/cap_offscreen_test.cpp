// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the viewer's cross-section solid cap (B2b). It renders a unit cube cut in half by a
// clip plane, twice — once with the cap, once without — through the same render_section_offscreen
// path the app uses, with a z-ramp field bound. Assertions: (1) the cap visibly changes the cut
// face (the centre pixel differs between capped and uncapped — the stencil-filled cap quad painted
// it), and (2) the capped section shows the field colormap (cold-blue and hot-red both present) —
// i.e. the field is drawn on the cut face (the slice). Off-screen + readback, GPU-free on lavapipe
// in CI.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "camera.hpp"
#include "cap.frag.spv.h"
#include "cap.hpp"
#include "cap.vert.spv.h"
#include "capmark.frag.spv.h"
#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "mesh_render.hpp"
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("viewer cross-section solid cap fills the cut face (B2b)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping cap render");
        return;
    }

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    const rime::viewer::CpuMesh cube = rime::viewer::make_unit_cube(); // x,y,z ∈ [-1, 1]
    const float r = cube.radius();

    // z-ramp field (value 0 bottom → 1 top), validity 1; 2×2×2 RGBA32F. (As in the colormap test.)
    std::vector<float> vol(2 * 2 * 2 * 4, 0.0f);
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const std::size_t gi = static_cast<std::size_t>(i) + 2 * (j + 2 * k);
                vol[gi * 4 + 0] = (k == 0) ? 0.0f : 1.0f;
                vol[gi * 4 + 1] = 1.0f;
                vol[gi * 4 + 3] = 1.0f;
            }

    rime::viewer::OrbitCamera cam;
    cam.frame(cube.center(), cube.radius(), 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    // Cut away the +x half; the cut face is the plane x = 0.
    rime::viewer::MeshPush mp{};
    mp.mvp = cam.view_proj(1.0f);
    const rime::core::Vec3 eye = cam.eye();
    mp.cam_pos[0] = eye.x;
    mp.cam_pos[1] = eye.y;
    mp.cam_pos[2] = eye.z;
    mp.cam_pos[3] = 1.0f;
    mp.clip_plane[0] = 1.0f; // normal +x
    mp.clip_plane[1] = 0.0f;
    mp.clip_plane[2] = 0.0f;
    mp.clip_plane[3] = 0.0f; // discard x > 0
    for (int c = 0; c < 3; ++c) {
        mp.field_scale[c] = 0.25f;
        mp.field_bias[c] = 0.5f;
    }
    mp.field_scale[3] = 0.0f; // vmin
    mp.field_bias[3] = 1.0f;  // vmax

    rime::viewer::CapPush cp{};
    cp.mvp = mp.mvp;
    for (int c = 0; c < 3; ++c) {
        cp.field_scale[c] = mp.field_scale[c];
        cp.field_bias[c] = mp.field_bias[c];
    }
    cp.field_scale[3] = 0.0f;
    cp.field_bias[3] = 1.0f;
    cp.cap_rect[0] = -r; // x-plane → in-plane axes are y, z
    cp.cap_rect[1] = r;
    cp.cap_rect[2] = -r;
    cp.cap_rect[3] = r;
    cp.cap_meta[0] = 0.0f; // offset
    cp.cap_meta[1] = 0.0f; // axis x

    const auto render = [&](bool do_cap) {
        return rime::viewer::render_section_offscreen(*device,
                                                      size,
                                                      cube,
                                                      mp,
                                                      cp,
                                                      clear,
                                                      do_cap,
                                                      mesh_vert_spv,
                                                      sizeof(mesh_vert_spv),
                                                      mesh_frag_spv,
                                                      sizeof(mesh_frag_spv),
                                                      capmark_frag_spv,
                                                      sizeof(capmark_frag_spv),
                                                      cap_vert_spv,
                                                      sizeof(cap_vert_spv),
                                                      cap_frag_spv,
                                                      sizeof(cap_frag_spv),
                                                      vol.data(),
                                                      2,
                                                      2,
                                                      2);
    };
    const std::vector<std::uint8_t> capped = render(true);
    const std::vector<std::uint8_t> uncapped = render(false);
    REQUIRE(capped.size() == static_cast<std::size_t>(size) * size * 4);

    const auto at = [&](const std::vector<std::uint8_t>& px, std::uint32_t x, std::uint32_t y) {
        return &px[(static_cast<std::size_t>(y) * size + x) * 4];
    };

    // (1) The cap changes the cut face: the centre pixel (on the x=0 cut, through the cube) differs
    // between capped and uncapped. Without the cap you see the lit interior back wall; with it, the
    // flat field-coloured cap.
    const std::uint8_t* cc = at(capped, size / 2, size / 2);
    const std::uint8_t* uc = at(uncapped, size / 2, size / 2);
    const int dr = cc[0] - uc[0], dg = cc[1] - uc[1], db = cc[2] - uc[2];
    CHECK((dr * dr + dg * dg + db * db) > 200); // a clear colour change at the cut

    // (2) The capped section shows the field colormap — cold (blue) and hot (red) both present —
    // and no collapsed-normal black holes.
    std::size_t part = 0, blue = 0, red = 0, black = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &capped[static_cast<std::size_t>(i) * 4];
        if (p[0] < 8 && p[1] < 8 && p[2] < 8)
            ++black;
        const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        if (lum <= 40.0f)
            continue;
        ++part;
        if (p[2] > p[0] + 25 && p[2] > p[1])
            ++blue;
        else if (p[0] > p[1] + 25 && p[0] > p[2] + 25)
            ++red;
    }
    CHECK(part > 0);
    CHECK(blue > 20);
    CHECK(red > 10);
    CHECK(black == 0);
}
