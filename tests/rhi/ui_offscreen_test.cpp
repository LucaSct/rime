// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the from-scratch immediate-mode UI (E2). Two halves:
//   * interaction (CPU only, always runs): a button fires on a click inside it, a checkbox toggles
//   its
//     bound bool, and dragging a slider sets its bound value — the hot/active-item logic works.
//   * render (off-screen, GPU): a panel with a label, a button, an on checkbox and a half slider
//   draws —
//     the panel fills its rect, bright text/widget pixels appear (the bitmap-font atlas path
//     works), and the checkbox's green tick shows. Off-screen + readback, GPU-free on lavapipe in
//     CI.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "ui.frag.spv.h"
#include "ui.hpp"
#include "ui.vert.spv.h"
#include "ui_render.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("ui: a button fires, a checkbox toggles, a slider drags (E2 interaction)") {
    using namespace rime::viewer::ui;
    // A press edge is mouse-down-this-frame after up-last-frame; a fresh Ui starts "up", so the
    // first mouse-down frame is the click. (Each sub-case uses its own Ui for that clean initial
    // state.)

    // A click at (30,30) lands in the first widget row of a panel pinned at (10,10): the button
    // fires.
    {
        Ui ui;
        ui.begin(800, 600, 30, 30, /*mouse_down=*/true);
        ui.panel(10, 10, 200, 300);
        const bool clicked = ui.button("GO");
        ui.end();
        CHECK(clicked);
    }
    // Same click, but the first widget is a checkbox bound to `flag`: it toggles false → true.
    {
        Ui ui;
        bool flag = false;
        ui.begin(800, 600, 22, 22, true);
        ui.panel(10, 10, 200, 300);
        const bool toggled = ui.checkbox("SHOW", flag);
        ui.end();
        CHECK(toggled);
        CHECK(flag == true);
    }
    // No press (mouse up) → the button does not fire.
    {
        Ui ui;
        ui.begin(800, 600, 30, 30, false);
        ui.panel(10, 10, 200, 300);
        const bool clicked_up = ui.button("GO");
        ui.end();
        CHECK_FALSE(clicked_up);
    }
    // Drag a slider: press at ~75% along the track (track x=18, width=184 ⇒ x≈156) sets
    // value≈75/100.
    {
        Ui ui;
        float value = 0.0f;
        ui.begin(800, 600, 156, 28, true);
        ui.panel(10, 10, 200, 300);
        const bool changed = ui.slider("EXPLODE", value, 0.0f, 100.0f);
        ui.end();
        CHECK(changed);
        CHECK(value == doctest::Approx(75.0f).epsilon(0.1));
    }
}

TEST_CASE("ui: a control panel renders — panel, text, widgets (E2 render)") {
    using namespace rime::rhi;
    namespace vui = rime::viewer::ui;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the UI render");
        return;
    }

    const std::uint32_t size = 256;
    vui::Ui ui;
    bool on = true;
    float v = 50.0f;
    ui.begin(static_cast<float>(size), static_cast<float>(size), -1.0f, -1.0f, false);
    ui.panel(8, 8, 200, 180);
    ui.label("ICEM VIEWER");
    (void)ui.button("RESET VIEW");
    (void)ui.checkbox("PLASMA", on); // on → a green tick
    (void)ui.slider("EXPLODE", v, 0.0f, 100.0f);
    ui.end();
    CHECK(ui.vertices().size() > 60); // panel + label + button + checkbox + slider = many quads

    const ClearColor clear{0.02f, 0.02f, 0.03f, 1.0f};
    const std::vector<std::uint8_t> px = vui::render_ui_offscreen(*device,
                                                                  size,
                                                                  ui,
                                                                  clear,
                                                                  ui_vert_spv,
                                                                  sizeof(ui_vert_spv),
                                                                  ui_frag_spv,
                                                                  sizeof(ui_frag_spv));
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    std::size_t panel = 0, bright = 0, green = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        if (p[0] <= 8 && p[1] <= 8 && p[2] <= 12)
            continue; // the dark clear background
        ++panel;
        if (p[0] > 150 && p[1] > 150 && p[2] > 150)
            ++bright; // light text / slider knob
        if (p[1] > 120 && p[1] > p[0] + 25 && p[1] > p[2] + 25)
            ++green; // the checkbox tick
    }
    CHECK(panel > 4000); // the opaque panel fills a chunk of the frame
    CHECK(bright > 80);  // text + the knob render (the bitmap-font atlas works)
    CHECK(green > 8);    // the on-checkbox's green tick
}
