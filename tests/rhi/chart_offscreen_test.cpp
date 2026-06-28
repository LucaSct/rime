// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the gas-path chart (D4) — the from-scratch 2-D line plot of a computed scalar field along the
// flow axis. Three halves:
//   * sampling (CPU, always): the axial-profile sampler picks the longest grid axis and averages each
//     cross-section slice, reproducing a known Mach distribution (inlet subsonic → exit supersonic).
//   * layout (CPU, always): build_gas_path_chart emits geometry (panel + axes + curve), and more of it
//     when the field crosses M = 1 (the sonic reference line is drawn).
//   * render (off-screen, GPU): the chart draws — panel fill, the amber curve, bright axis labels. Set
//     RIME_DUMP_CHART=<path.ppm> to also write the frame for eyeballing. GPU-free-skippable in CI.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "chart.hpp"
#include "field.hpp"
#include "rime/rhi/rhi.hpp"
#include "ui.frag.spv.h"
#include "ui.hpp"
#include "ui.vert.spv.h"
#include "ui_render.hpp"

namespace {

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// A synthetic nozzle Mach field: a slender grid (long along z), Mach rising linearly from a subsonic
// inlet to a supersonic exit, every voxel valid — the shape brick26 produces, without needing ICEM.
rime::viewer::ScalarField demo_mach_field() {
    rime::viewer::ScalarField f;
    f.name = "mach";
    f.unit = "-";
    f.nx = 5;
    f.ny = 5;
    f.nz = 40; // the flow axis (longest)
    f.vmin = 0.2f;
    f.vmax = 2.5f;
    f.rgba.assign(static_cast<std::size_t>(f.nx) * f.ny * f.nz * 4, 0.0f);
    for (std::uint32_t k = 0; k < f.nz; ++k) {
        const float m = 0.2f + 2.3f * static_cast<float>(k) / static_cast<float>(f.nz - 1);
        for (std::uint32_t j = 0; j < f.ny; ++j)
            for (std::uint32_t i = 0; i < f.nx; ++i) {
                const std::size_t g =
                    (static_cast<std::size_t>(i) + f.nx * (j + static_cast<std::size_t>(f.ny) * k)) * 4;
                f.rgba[g + 0] = m;    // R = value
                f.rgba[g + 1] = 1.0f; // G = valid
            }
    }
    return f;
}

} // namespace

TEST_CASE("chart: the axial-profile sampler averages slices down the flow axis (D4)") {
    using namespace rime::viewer;
    const ScalarField f = demo_mach_field();
    int axis = -1;
    const std::vector<float> prof = sample_axial_profile(f, &axis);

    CHECK(axis == 2);                  // the longest axis (nz) is the flow direction
    REQUIRE(prof.size() == f.nz);
    CHECK(prof.front() == doctest::Approx(0.2f));  // subsonic inlet
    CHECK(prof.back() == doctest::Approx(2.5f));   // supersonic exit
    // Monotone increasing, and it crosses M = 1 somewhere in between.
    bool monotone = true, crosses_sonic = false;
    for (std::size_t i = 1; i < prof.size(); ++i) {
        if (prof[i] < prof[i - 1] - 1e-6f)
            monotone = false;
        if (prof[i - 1] < 1.0f && prof[i] >= 1.0f)
            crosses_sonic = true;
    }
    CHECK(monotone);
    CHECK(crosses_sonic);
}

TEST_CASE("chart: the layout emits geometry, and the sonic line only when M=1 is in range (D4)") {
    using namespace rime::viewer;
    const ScalarField f = demo_mach_field();

    ui::Ui gui;
    gui.begin(900, 600, -1.0f, -1.0f, false);
    build_gas_path_chart(gui, f, 20.0f, 360.0f, 460.0f, 210.0f);
    gui.end();
    const std::size_t with_sonic = gui.vertices().size();
    CHECK(with_sonic > 200); // panel + axes + a 40-segment curve + labels + the dashed M=1 line

    // The same field clamped to a fully-supersonic range never crosses M = 1 → no sonic reference line,
    // so strictly fewer vertices than the crossing case above.
    ScalarField supersonic = f;
    supersonic.vmin = 1.5f;
    supersonic.vmax = 2.5f;
    ui::Ui gui2;
    gui2.begin(900, 600, -1.0f, -1.0f, false);
    build_gas_path_chart(gui2, supersonic, 20.0f, 360.0f, 460.0f, 210.0f);
    gui2.end();
    CHECK(gui2.vertices().size() < with_sonic);
}

TEST_CASE("chart: it renders — panel, the amber curve, bright labels (D4 render)") {
    using namespace rime::rhi;
    namespace vv = rime::viewer;
    namespace vui = rime::viewer::ui;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping the chart render");
        return;
    }

    const std::uint32_t size = 320;
    const vv::ScalarField f = demo_mach_field();
    vui::Ui gui;
    gui.begin(static_cast<float>(size), static_cast<float>(size), -1.0f, -1.0f, false);
    vv::build_gas_path_chart(gui, f, 8.0f, 80.0f, static_cast<float>(size) - 16.0f, 180.0f);
    gui.end();
    CHECK(gui.vertices().size() > 200);

    const ClearColor clear{0.10f, 0.11f, 0.13f, 1.0f};
    const std::vector<std::uint8_t> px = vui::render_ui_offscreen(
        *device, size, gui, clear, ui_vert_spv, sizeof(ui_vert_spv), ui_frag_spv, sizeof(ui_frag_spv));
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    std::size_t panel = 0, bright = 0, amber = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        if (p[0] <= 18 && p[1] <= 20 && p[2] <= 26)
            continue; // clear / panel background
        ++panel;
        if (p[0] > 150 && p[1] > 150 && p[2] > 150)
            ++bright; // axis labels
        if (p[0] > 180 && p[1] > 140 && p[2] < 130)
            ++amber; // the curve (R,G high, B low)
    }
    CHECK(panel > 1500);
    CHECK(bright > 20);
    CHECK(amber > 30);

    // Optional eyeball dump: RIME_DUMP_CHART=/path/chart.ppm
    if (const char* dump = std::getenv("RIME_DUMP_CHART")) {
        std::ofstream out(dump, std::ios::binary);
        out << "P6\n" << size << ' ' << size << "\n255\n";
        for (std::uint32_t i = 0; i < size * size; ++i)
            out.write(reinterpret_cast<const char*>(&px[static_cast<std::size_t>(i) * 4]), 3);
        MESSAGE("wrote chart dump to ", dump);
    }
}
