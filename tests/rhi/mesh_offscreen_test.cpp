// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the viewer's lit mesh pass (B1): it renders a unit cube off-screen through the same
// make_mesh/record_mesh/render_mesh_offscreen path the app uses, then asserts the part is actually
// there and lit — the center is brighter than the dark background, a corner is the background, the
// part covers a sensible fraction of the frame, and there is no swath of NaN-black pixels (the failure
// mode when normals collapse). Exercises depth + push-constant MVP + the lit shader end to end.
// Off-screen + readback, so it runs GPU-free on lavapipe in CI like the other RHI proofs.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"

#include "camera.hpp"      // samples/03-icem-viewer (on the include path; see tests/rhi/CMakeLists.txt)
#include "mesh_render.hpp"
#include "stl.hpp"

#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
float luminance(const std::uint8_t* p) {
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}
} // namespace

TEST_CASE("viewer renders a lit cube off-screen") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping mesh render");
        return;
    }

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f}; // dark background

    const rime::viewer::CpuMesh cube = rime::viewer::make_unit_cube();

    // Frame the cube from a three-quarter view, exactly as the app's setup does.
    rime::viewer::OrbitCamera cam;
    cam.frame(cube.center(), cube.radius(), 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    rime::viewer::MeshPush push{};
    push.mvp = cam.view_proj(1.0f);
    const rime::core::Vec3 eye = cam.eye();
    push.cam_pos[0] = eye.x;
    push.cam_pos[1] = eye.y;
    push.cam_pos[2] = eye.z;
    push.cam_pos[3] = 1.0f;

    const std::vector<std::uint8_t> px = rime::viewer::render_mesh_offscreen(*device,
                                                                            size,
                                                                            cube,
                                                                            push,
                                                                            clear,
                                                                            mesh_vert_spv,
                                                                            sizeof(mesh_vert_spv),
                                                                            mesh_frag_spv,
                                                                            sizeof(mesh_frag_spv));
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return &px[(static_cast<std::size_t>(y) * size + x) * 4];
    };

    // The framed cube covers the center: it should be clearly lit, well above the dark background.
    CHECK(luminance(at(size / 2, size / 2)) > 60.0f);
    // A corner is outside the part: it stays the (dark) background.
    CHECK(luminance(at(1, 1)) < 40.0f);

    // Coverage is sensible, and there is no field of NaN-black pixels (the collapsed-normal failure).
    std::size_t lit = 0, black = 0;
    float max_lum = 0.0f;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        const float l = luminance(p);
        if (l > 40.0f) ++lit;
        if (p[0] < 8 && p[1] < 8 && p[2] < 8) ++black; // darker than the background → NaN/garbage
        if (l > max_lum) max_lum = l;
    }
    const double frac = static_cast<double>(lit) / (static_cast<double>(size) * size);
    CHECK(frac > 0.10); // the part is actually on screen
    CHECK(frac < 0.95); // ...and so is some background
    CHECK(max_lum > 150.0f); // real shading reaches bright highlights
    CHECK(black == 0);       // no collapsed-normal black holes
}
