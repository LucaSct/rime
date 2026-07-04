// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for multi-part assemblies (E1). A machine like ICEM's tokamak is a set of *concentric*
// shells (vessel ⊃ blanket ⊃ first wall ⊃ plasma), so this builds three nested cubes — a big red
// one, a medium blue one, a small green one, all about the origin — and checks the three things an
// assembly view must do:
//   1. per-part colour: assembled, the outer (red) part is what you see.
//   2. per-part visibility: hide the outer part and the next one (blue) is revealed; the red is
//   gone.
//   3. exploded view: fan the parts apart and all three colours are visible at once — the axial fan
//      separates even perfectly concentric shells (whose radial offset is ~0), and the framing
//      radius grows. Parts are classified by dominant colour channel (robust to the studio
//      lighting).
// Off-screen + readback, GPU-free on lavapipe in CI.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "assembly.hpp"
#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "mesh_render.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/render/orbit_camera.hpp" // graduated from the viewer at M5.5
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// A cube of half-size `half` centred on the origin, as a flat-shaded part.
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

// Count non-background pixels whose dominant channel matches red / blue / green (margin 15/255).
struct Hues {
    std::size_t red = 0, blue = 0, green = 0, lit = 0;
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
        if (p[1] > p[0] + 15 && p[1] > p[2] + 15)
            ++h.green;
    }
    return h;
}
} // namespace

TEST_CASE("viewer draws a multi-part assembly — colour, visibility, exploded view (E1)") {
    using namespace rime::rhi;

    // Three concentric cubes: red (half 1.0) ⊃ blue (0.6) ⊃ green (0.3). Colour is by load order,
    // so part 0 = red, 1 = blue, 2 = green (the first three palette entries).
    rime::viewer::Assembly a;
    a.parts.push_back(cube_part(1.0f, "outer"));
    a.parts.push_back(cube_part(0.6f, "middle"));
    a.parts.push_back(cube_part(0.3f, "inner"));
    rime::viewer::finalize_assembly(a);

    CHECK(a.parts.size() == 3);
    CHECK(a.center.z == doctest::Approx(0.0f));
    // Concentric → the radial offset is ~0; the axial fan must still separate them, so the framing
    // radius has to grow when exploded.
    const float r0 = rime::viewer::framing_radius(a, 0.0f);
    const float r1 = rime::viewer::framing_radius(a, 1.0f);
    CHECK(r1 > r0 + 0.5f);

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the assembly render");
        return;
    }

    const std::uint32_t size = 160;
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};
    const auto render = [&](float factor) {
        rime::render::OrbitCamera cam;
        cam.frame(a.center, rime::viewer::framing_radius(a, factor), 1.0f);
        cam.yaw = rime::core::radians(35.0f);
        cam.pitch = rime::core::radians(20.0f);
        return rime::viewer::render_assembly_offscreen(*device,
                                                       size,
                                                       a,
                                                       cam.view_proj(1.0f),
                                                       cam.eye(),
                                                       factor,
                                                       clear,
                                                       mesh_vert_spv,
                                                       sizeof(mesh_vert_spv),
                                                       mesh_frag_spv,
                                                       sizeof(mesh_frag_spv));
    };

    // (1) Assembled: the outer red shell occludes the rest — red dominates, little/no blue or
    // green.
    Hues asm_h = classify(render(0.0f), size);
    CHECK(asm_h.red > 300);
    CHECK(asm_h.red > asm_h.blue + asm_h.green);

    // (2) Hide the outer part: the blue middle shell is revealed, and the red is gone.
    a.parts[0].visible = false;
    Hues hid_h = classify(render(0.0f), size);
    CHECK(hid_h.blue > 200);
    CHECK(hid_h.red < 60);
    a.parts[0].visible = true;

    // (3) Exploded: the parts fan apart along the axis, so all three colours show at once.
    Hues exp_h = classify(render(1.0f), size);
    CHECK(exp_h.red > 80);
    CHECK(exp_h.blue > 80);
    CHECK(exp_h.green > 80);
}
