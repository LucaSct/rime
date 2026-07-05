// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the viewer's lit mesh pass (B1): it renders a unit cube off-screen through the same
// make_mesh/record_mesh/render_mesh_offscreen path the app uses, then asserts the part is actually
// there and lit — the center is brighter than the dark background, a corner is the background, the
// part covers a sensible fraction of the frame, and there is no swath of NaN-black pixels (the
// failure mode when normals collapse). Exercises depth + push-constant MVP + the lit shader end to
// end. Off-screen + readback, so it runs GPU-free on lavapipe in CI like the other RHI proofs.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "mesh_render.hpp"
#include "rime/render/orbit_camera.hpp" // graduated from the viewer at M5.5
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"

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
    rime::render::OrbitCamera cam;
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

    // Coverage is sensible, and there is no field of NaN-black pixels (the collapsed-normal
    // failure).
    std::size_t lit = 0, black = 0;
    float max_lum = 0.0f;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        const float l = luminance(p);
        if (l > 40.0f)
            ++lit;
        if (p[0] < 8 && p[1] < 8 && p[2] < 8)
            ++black; // darker than the background → NaN/garbage
        if (l > max_lum)
            max_lum = l;
    }
    const double frac = static_cast<double>(lit) / (static_cast<double>(size) * size);
    CHECK(frac > 0.10);      // the part is actually on screen
    CHECK(frac < 0.95);      // ...and so is some background
    CHECK(max_lum > 150.0f); // real shading reaches bright highlights
    CHECK(black == 0);       // no collapsed-normal black holes
}

TEST_CASE("viewer cross-section clips a half-space and reveals the interior") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping cross-section render");
        return;
    }

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    const rime::viewer::CpuMesh cube = rime::viewer::make_unit_cube(); // x,y,z ∈ [-1, 1]

    rime::render::OrbitCamera cam;
    cam.frame(cube.center(), cube.radius(), 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    // Render with a given clip plane and return (lit fraction, center luminance).
    const auto render = [&](float nx, float ny, float nz, float w) {
        rime::viewer::MeshPush push{};
        push.mvp = cam.view_proj(1.0f);
        const rime::core::Vec3 e = cam.eye();
        push.cam_pos[0] = e.x;
        push.cam_pos[1] = e.y;
        push.cam_pos[2] = e.z;
        push.cam_pos[3] = 1.0f;
        push.clip_plane[0] = nx;
        push.clip_plane[1] = ny;
        push.clip_plane[2] = nz;
        push.clip_plane[3] = w;
        const std::vector<std::uint8_t> px =
            rime::viewer::render_mesh_offscreen(*device,
                                                size,
                                                cube,
                                                push,
                                                clear,
                                                mesh_vert_spv,
                                                sizeof(mesh_vert_spv),
                                                mesh_frag_spv,
                                                sizeof(mesh_frag_spv));
        std::size_t lit = 0;
        for (std::uint32_t i = 0; i < size * size; ++i) {
            if (luminance(&px[static_cast<std::size_t>(i) * 4]) > 40.0f)
                ++lit;
        }
        const float center =
            luminance(&px[(static_cast<std::size_t>(size / 2) * size + size / 2) * 4]);
        return std::pair<double, float>{static_cast<double>(lit) / (size * size), center};
    };

    const auto none = render(0.0f, 0.0f, 0.0f, 0.0f); // dot=0 never exceeds 0 → clips nothing
    const auto half = render(1.0f, 0.0f, 0.0f, 0.0f); // discard x > 0 → keep the −x half
    const auto allc =
        render(1.0f, 0.0f, 0.0f, -1.5f); // discard x > −1.5 → the whole cube is removed

    // The section removes geometry: a half cut shows less than the whole, and cutting past the part
    // shows essentially nothing.
    CHECK(half.first < none.first);
    CHECK(half.first > 0.02); // ...but the remaining half is still clearly on screen
    CHECK(allc.first < 0.01); // cutting beyond the part empties the frame

    // The cut reveals the interior rather than punching a hole to the background: the kept half
    // still lights up (two-sided shading on the now-exposed inner faces).
    CHECK(half.second > 40.0f);
}

TEST_CASE("viewer colours the part by a field (colormap, C1)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping field render");
        return;
    }

    const std::uint32_t size = 128;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    const rime::viewer::CpuMesh cube = rime::viewer::make_unit_cube(); // x,y,z ∈ [-1, 1]

    // A 2×2×2 field that ramps along z: value 0 on the bottom slice (→ blue), 1 on the top (→ red);
    // validity 1 everywhere. node-major index i + 2*(j + 2*k).
    std::vector<float> vol(2 * 2 * 2 * 4, 0.0f);
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const std::size_t gi = static_cast<std::size_t>(i) + 2 * (j + 2 * k);
                vol[gi * 4 + 0] = (k == 0) ? 0.0f : 1.0f; // value
                vol[gi * 4 + 1] = 1.0f;                   // validity
                vol[gi * 4 + 3] = 1.0f;
            }

    rime::render::OrbitCamera cam;
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
    push.clip_plane[0] = push.clip_plane[1] = push.clip_plane[2] = 0.0f;
    push.clip_plane[3] = 1e30f; // no clip
    // World [-1,1]³ → uvw with a 2-node axis: scale = 1/(h·n) = 0.25, bias = 0.5/n − origin/(h·n) =
    // 0.5.
    for (int c = 0; c < 3; ++c) {
        push.field_scale[c] = 0.25f;
        push.field_bias[c] = 0.5f;
    }
    push.field_scale[3] = 0.0f; // vmin
    push.field_bias[3] = 1.0f;  // vmax  (vmax > vmin → field on)

    const std::vector<std::uint8_t> px = rime::viewer::render_mesh_offscreen(*device,
                                                                             size,
                                                                             cube,
                                                                             push,
                                                                             clear,
                                                                             mesh_vert_spv,
                                                                             sizeof(mesh_vert_spv),
                                                                             mesh_frag_spv,
                                                                             sizeof(mesh_frag_spv),
                                                                             vol.data(),
                                                                             2,
                                                                             2,
                                                                             2);
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    // The colormap must put both ends on the part: cold (blue-dominant) and hot (red-dominant)
    // pixels. A plain metal shade (the field-off path) would have neither.
    std::size_t part = 0, blue = 0, red = 0, black = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        if (p[0] < 8 && p[1] < 8 && p[2] < 8)
            ++black;
        if (luminance(p) <= 40.0f)
            continue; // background
        ++part;
        if (p[2] > p[0] + 25 && p[2] > p[1])
            ++blue; // blue/cyan-dominant → cold end
        else if (p[0] > p[1] + 25 && p[0] > p[2] + 25)
            ++red; // red-dominant → hot end
    }
    CHECK(part > 0);
    CHECK(blue > 30); // the cold end is clearly shown
    CHECK(red > 10);  // ...and so is the hot end → a real value range, not a flat colour
    CHECK(black == 0);
}
