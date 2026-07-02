// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the provenance panel (E3) — surfacing ICEM's "why" Ledger (.icejson) on the
// from-scratch UI. Three halves:
//   * parser (CPU only, always runs): the line-based reader pulls the header (design / hash /
//   pass-fail)
//     and every node (origin / label / value / unit / causal inputs) out of a small .icejson, and
//     rejects a file that is not one. This is the contract with ICEM's `know::provenance_json`,
//     shared by file format only (rime repo docs/math/provenance-panel.md; icem repo
//     docs/math/provenance-io.md).
//   * layout (CPU only, always runs): clicking a row selects it, clicking it again clears it, and a
//     selected node with causes expands its derivation — so the panel grows by those sub-rows.
//   * render (off-screen, GPU): the panel draws — its fill covers the frame, bright text pixels
//   appear
//     (the bitmap-font path), and the green of the PASS verdict + the CHK origin tag shows.
//     GPU-free on lavapipe in CI; skipped (not failed) when no device, unless RIME_REQUIRE_VULKAN
//     is set.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "provenance.hpp"
#include "provenance_view.hpp"
#include "rime/rhi/rhi.hpp"
#include "ui.frag.spv.h"
#include "ui.hpp"
#include "ui.vert.spv.h"
#include "ui_render.hpp"

namespace {

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// A tiny synthetic ledger covering one of each major origin (Input / Material / Law / Rule /
// Derived), so the layout + render proofs exercise the full origin-tag / colour vocabulary without
// touching ICEM.
rime::viewer::Provenance demo_ledger() {
    using rime::viewer::ProvNode;
    rime::viewer::Provenance p;
    p.design = "demo widget";
    p.hash = "abc123def4567890";
    p.passes = true;
    p.nodes.push_back(ProvNode{0, "Input", "chamber pressure", 6.0e6, "Pa", {}});
    p.nodes.push_back(ProvNode{1, "Material", "yield strength", 1.1e9, "Pa", {}});
    p.nodes.push_back(ProvNode{2, "Law", "hoop stress = p r / t", 6.429e8, "Pa", {0}});
    p.nodes.push_back(ProvNode{3, "Rule", "structural: yield / hoop", 1.218, "-", {1, 2}});
    p.nodes.push_back(ProvNode{4, "Derived", "verdict: adequate", 1.0, "bool", {3}});
    return p;
}

} // namespace

TEST_CASE(
    "provenance: the .icejson reader parses header + nodes, rejects non-provenance (E3 parse)") {
    using namespace rime::viewer;

    // ICEM writes one node object per line inside a one-line header object (no JSON library
    // needed).
    const std::string text =
        "{\"kind\":\"icem-provenance\",\"version\":1,\"design\":\"demo widget\","
        "\"hash\":\"abc123def4567890\",\"passes\":true,\"nodes\":[\n"
        "{\"id\":0,\"origin\":\"Input\",\"label\":\"vessel inner radius\","
        "\"value\":0.074999999999999997,\"unit\":\"m\",\"inputs\":[]},\n"
        "{\"id\":1,\"origin\":\"Law\",\"label\":\"hoop stress = p r / t\","
        "\"value\":642900000,\"unit\":\"Pa\",\"inputs\":[0]},\n"
        "{\"id\":2,\"origin\":\"Rule\",\"label\":\"structural margin\","
        "\"value\":1.218,\"unit\":\"-\",\"inputs\":[1]}\n"
        "]}\n";
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "rime_provenance_test.icejson";
    {
        std::ofstream o(tmp, std::ios::binary);
        o << text;
    }

    auto prov = load_provenance(tmp.string());
    REQUIRE(prov.has_value());
    CHECK(prov->design == "demo widget");
    CHECK(prov->hash == "abc123def4567890");
    CHECK(prov->passes == true);
    REQUIRE(prov->nodes.size() == 3);

    // An Input node: no causes, full round-trip value.
    CHECK(prov->nodes[0].origin == "Input");
    CHECK(prov->nodes[0].label == "vessel inner radius");
    CHECK(prov->nodes[0].value == doctest::Approx(0.075).epsilon(1e-12));
    CHECK(prov->nodes[0].unit == "m");
    CHECK(prov->nodes[0].inputs.empty());

    // A Law node: its single cause is the earlier node's id (the DAG edge the panel walks).
    CHECK(prov->nodes[1].origin == "Law");
    CHECK(prov->nodes[1].label == "hoop stress = p r / t");
    CHECK(prov->nodes[1].value == doctest::Approx(6.429e8));
    REQUIRE(prov->nodes[1].inputs.size() == 1);
    CHECK(prov->nodes[1].inputs[0] == 0u);
    CHECK(prov->usable());

    // A file that is not an icem-provenance export is rejected (nullopt), not misparsed.
    const std::filesystem::path bad = std::filesystem::temp_directory_path() / "rime_not_prov.txt";
    {
        std::ofstream o(bad, std::ios::binary);
        o << "solid teapot\nfacet normal 0 0 1\n";
    }
    CHECK_FALSE(load_provenance(bad.string()).has_value());
    CHECK_FALSE(load_provenance("/no/such/file.icejson").has_value());

    std::filesystem::remove(tmp);
    std::filesystem::remove(bad);
}

TEST_CASE(
    "provenance: clicking a row selects it; a selected node expands its derivation (E3 layout)") {
    using namespace rime::viewer;
    const Provenance prov = demo_ledger();
    const float W = 700.0f, H = 600.0f;
    // Mirror the panel's own geometry so the click lands squarely on the first row.
    const float row_h = ui::kGlyphH * 1.6f + 8.0f;
    const float head_h = 86.0f;
    const float row0_y = head_h + row_h * 0.5f;

    // No selection: the list lays out (roughly one row per node), and content is positive.
    {
        ui::Ui gui;
        int sel = -1;
        gui.begin(W, H, -1.0f, -1.0f, /*mouse_down=*/false);
        const float content = build_provenance_panel(gui, prov, sel, 0.0f, W, H);
        gui.end();
        CHECK(sel == -1);
        CHECK(content >= row_h * static_cast<float>(prov.nodes.size()));
    }

    // A press over the first row selects node 0 (the rising mouse-down edge is the click).
    {
        ui::Ui gui;
        int sel = -1;
        gui.begin(W, H, 120.0f, row0_y, /*mouse_down=*/true);
        build_provenance_panel(gui, prov, sel, 0.0f, W, H);
        gui.end();
        CHECK(sel == 0);
    }

    // Clicking the already-selected row again clears the selection (toggle off).
    {
        ui::Ui gui;
        int sel = 0;
        gui.begin(W, H, 120.0f, row0_y, /*mouse_down=*/true);
        build_provenance_panel(gui, prov, sel, 0.0f, W, H);
        gui.end();
        CHECK(sel == -1);
    }

    // Selecting the verdict (which has causes) expands its derivation, so the panel grows by those
    // sub-rows versus nothing selected. No click here — we drive `selected` directly.
    float base = 0.0f, expanded = 0.0f;
    {
        ui::Ui gui;
        int none = -1;
        gui.begin(W, H, -1.0f, -1.0f, false);
        base = build_provenance_panel(gui, prov, none, 0.0f, W, H);
        gui.end();
    }
    {
        ui::Ui gui;
        int verdict = static_cast<int>(prov.nodes.size()) - 1; // node 4 ← {3}
        gui.begin(W, H, -1.0f, -1.0f, false);
        expanded = build_provenance_panel(gui, prov, verdict, 0.0f, W, H);
        gui.end();
        CHECK(verdict == static_cast<int>(prov.nodes.size()) - 1); // unchanged (no click)
    }
    CHECK(expanded > base);
}

TEST_CASE("provenance: the panel renders — fill, text, the PASS/CHK greens (E3 render)") {
    using namespace rime::rhi;
    namespace vv = rime::viewer;
    namespace vui = rime::viewer::ui;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the provenance render");
        return;
    }

    const std::uint32_t size = 512;
    const vv::Provenance prov = demo_ledger();
    vui::Ui gui;
    int selected = static_cast<int>(prov.nodes.size()) - 1; // expand the verdict's derivation
    gui.begin(static_cast<float>(size), static_cast<float>(size), -1.0f, -1.0f, false);
    vv::build_provenance_panel(
        gui, prov, selected, 0.0f, static_cast<float>(size), static_cast<float>(size));
    gui.end();
    CHECK(gui.vertices().size() > 100); // header band + many rows + the expanded derivation

    const ClearColor clear{0.06f, 0.07f, 0.09f, 1.0f};
    const std::vector<std::uint8_t> px = vui::render_ui_offscreen(*device,
                                                                  size,
                                                                  gui,
                                                                  clear,
                                                                  ui_vert_spv,
                                                                  sizeof(ui_vert_spv),
                                                                  ui_frag_spv,
                                                                  sizeof(ui_frag_spv));
    REQUIRE(px.size() == static_cast<std::size_t>(size) * size * 4);

    std::size_t panel = 0, bright = 0, green = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &px[static_cast<std::size_t>(i) * 4];
        if (p[0] <= 10 && p[1] <= 12 && p[2] <= 16)
            continue; // the dark clear / background fill edges
        ++panel;
        if (p[0] > 150 && p[1] > 150 && p[2] > 150)
            ++bright; // light row text
        if (p[1] > 120 && p[1] > p[0] + 30 && p[1] > p[2] + 30)
            ++green; // the PASS verdict + the CHK origin tag
    }
    CHECK(panel > 4000); // the opaque panel covers a large part of the frame
    CHECK(bright > 80);  // the bitmap-font rows render
    CHECK(green > 8);    // PASS (header) + the green Rule/CHK tag
}
