// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the engine cut-away (Bview): ICEM's geared turbofan shown as the sectioned, coloured
// assembly AND its computed flow in one pass. A synthetic stand-in for what `icem engine` emits — a
// solid casing cube plus a vec3 velocity field flowing along +z with a *Mach* scalar that ramps
// from 0.2 at the inlet to 0.9 at the outlet — exercises the three new things the view must do:
//   1. Mach colouring (data): build_streamlines, given the Mach scalar + a reference, colours each
//      vertex by Mach/Mach_ref, so the w channel spans cool (≈0.22) at the inlet to hot (1.0) at
//      the outlet — i.e. the lines read true Mach, not raw speed.
//   2. combined render: the cut-away assembly and the streamlines draw into one shared scope, so
//   both
//      land on screen at once (the casing is red; the cool inlet streamlines are unambiguously
//      blue).
//   3. cut-away: switching the clip plane on removes solid, so less of the red casing is drawn than
//      with the whole engine.
// Off-screen + readback, GPU-free on lavapipe in CI.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "assembly.hpp"
#include "camera.hpp"
#include "engine.hpp"
#include "field.hpp"
#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "mesh_render.hpp"
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"
#include "streamline.frag.spv.h"
#include "streamline.vert.spv.h"
#include "streamlines.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// A flat-shaded cube of half-size `half` about the origin — the engine "casing" the cut opens.
rime::viewer::Part cube_part(float half, std::string name) {
    rime::viewer::Part p;
    p.name = std::move(name);
    p.mesh = rime::viewer::make_unit_cube(); // [-1,1]
    p.mesh.bb_min = {1e30f, 1e30f, 1e30f};
    p.mesh.bb_max = {-1e30f, -1e30f, -1e30f};
    for (auto& v : p.mesh.vertices) {
        v.px *= half;
        v.py *= half;
        v.pz *= half;
        rime::viewer::detail::expand_bounds(p.mesh, {v.px, v.py, v.pz});
    }
    return p;
}

// Count background / red / blue pixels (dominant-channel classification, robust to studio
// lighting).
struct Hues {
    std::size_t lit = 0, red = 0, blue = 0;
};

Hues classify(const std::vector<std::uint8_t>& px, std::uint32_t size) {
    Hues h{};
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        if (p[0] <= 40 && p[1] <= 40 && p[2] <= 40)
            continue; // background
        ++h.lit;
        if (p[0] > p[1] + 15 && p[0] > p[2] + 15)
            ++h.red;
        if (p[2] > p[0] + 15 && p[2] > p[1] + 15)
            ++h.blue;
    }
    return h;
}
} // namespace

TEST_CASE("viewer fuses the cut-away assembly with Mach-coloured streamlines (Bview)") {
    using namespace rime::rhi;

    const std::uint32_t n = 8;
    const float h = 2.0f / (n - 1); // the field cube spans [-1,1]

    // A vec3 velocity flowing along +z and speeding up downstream (v_z 0.5 -> 2.0).
    rime::viewer::VectorField vf;
    vf.name = "velocity";
    vf.nx = vf.ny = vf.nz = n;
    vf.vmag_max = 2.0f;
    // The Mach scalar on the *same* grid: 0.2 at the inlet (low z) -> 0.9 at the outlet (high z).
    rime::viewer::ScalarField mf;
    mf.name = "mach";
    mf.nx = mf.ny = mf.nz = n;
    mf.vmin = 0.2f;
    mf.vmax = 0.9f;
    for (int c = 0; c < 3; ++c) {
        vf.scale[c] = 1.0f / (h * n);
        vf.bias[c] = 0.5f / n - (-1.0f) / (h * n);
        mf.scale[c] = vf.scale[c];
        mf.bias[c] = vf.bias[c];
    }
    vf.rgba.assign(static_cast<std::size_t>(n) * n * n * 4, 0.0f);
    mf.rgba.assign(static_cast<std::size_t>(n) * n * n * 4, 0.0f);
    for (std::uint32_t k = 0; k < n; ++k)
        for (std::uint32_t j = 0; j < n; ++j)
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::size_t gi = (i + n * (j + static_cast<std::size_t>(n) * k)) * 4;
                vf.rgba[gi + 2] = 0.5f + 1.5f * static_cast<float>(k) / (n - 1); // v_z
                vf.rgba[gi + 3] = 1.0f;                                          // validity
                mf.rgba[gi + 0] = 0.2f + 0.7f * static_cast<float>(k) / (n - 1); // Mach value
                mf.rgba[gi + 1] = 1.0f;                                          // validity
            }

    // (1) Mach colouring at the data level: the lines are coloured by Mach/Mach_max, so w spans the
    // cool inlet to the hot outlet — independent of any GPU.
    const float mach_max = mf.vmax;
    const std::vector<float> lines = rime::viewer::build_streamlines(vf, &mf, mach_max, 10);
    REQUIRE(lines.size() > 32);
    REQUIRE(lines.size() % 4 == 0);
    float wmin = 1e9f, wmax = -1e9f;
    for (std::size_t v = 0; v < lines.size(); v += 4) {
        wmin = std::min(wmin, lines[v + 3]);
        wmax = std::max(wmax, lines[v + 3]);
    }
    CHECK(wmin < 0.4f); // a cool (low-Mach) inlet end
    CHECK(wmax > 0.8f); // a hot (high-Mach) outlet end

    rime::viewer::EngineScene scene;
    scene.assembly.parts.push_back(cube_part(1.0f, "casing"));
    rime::viewer::finalize_assembly(scene.assembly);
    scene.core_lines = lines;
    scene.mach_max = mach_max;
    scene.has_flow = true;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping the engine render");
        return;
    }

    const std::uint32_t size = 160;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    rime::viewer::OrbitCamera cam;
    cam.frame(scene.assembly.center, rime::viewer::framing_radius(scene.assembly, 0.0f), 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    const auto render = [&](bool cutaway) {
        const std::array<float, 4> clip =
            rime::viewer::make_clip_plane(cutaway, 1, 1.0f, scene.assembly.center.y);
        return rime::viewer::render_engine_offscreen(*device,
                                                     size,
                                                     scene,
                                                     cam.view_proj(1.0f),
                                                     cam.eye(),
                                                     clip,
                                                     0.0f,
                                                     clear,
                                                     mesh_vert_spv,
                                                     sizeof(mesh_vert_spv),
                                                     mesh_frag_spv,
                                                     sizeof(mesh_frag_spv),
                                                     streamline_vert_spv,
                                                     sizeof(streamline_vert_spv),
                                                     streamline_frag_spv,
                                                     sizeof(streamline_frag_spv));
    };

    const Hues cut = classify(render(true), size);
    const Hues whole = classify(render(false), size);

    // (2) Combined render: both land on screen — the casing (red) and, in the opened half, the cool
    // inlet streamlines (blue, a colour the red casing never produces).
    CHECK(cut.lit > 200);
    CHECK(cut.red > 80);  // the sectioned casing
    CHECK(cut.blue > 10); // the Mach-coloured streamlines the cut reveals

    // (3) Cut-away: switching the clip plane on removes solid, so less red casing is drawn than for
    // the whole engine.
    CHECK(whole.red > cut.red + 50);
}
