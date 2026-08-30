// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.3b — the HUD overlay, on pixels. Needs a device, not a window, so lavapipe runs it in CI.
//
// Three claims, and the third is the one that matters most:
//
//   1. Text ACTUALLY DRAWS — a HUD that silently renders nothing looks exactly like a HUD with
//      nothing to say, so the proof measures coverage inside the text's own bounding box.
//   2. Glyphs are DISTINGUISHABLE — an 'M' covers more of its cell than a '.' does. A pass that
//      drew the same blob for every character would satisfy (1) perfectly.
//   3. The overlay does not disturb the frame UNDERNEATH it. It loads and blends over a finished
//      image, so every pixel outside what it drew must come back byte-identical. This is what makes
//      it an overlay rather than a second renderer, and it is the property that quietly breaks the
//      day someone gives the pass a depth attachment or the wrong load op.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/text/hud.hpp"

using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

namespace {

constexpr std::uint32_t kW = 256;
constexpr std::uint32_t kH = 128;

// Fill a target with a known colour so "did the overlay change this pixel" is answerable exactly.
void fill(RenderGraph& graph, RGTexture target, float r, float g, float b) {
    const RGColorAttachment colors[] = {
        {target, rhi::LoadOp::Clear, rhi::StoreOp::Store, {r, g, b, 1.0f}}};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    graph.add_raster_pass("fill", desc, [](rhi::CommandBuffer&) {});
}

[[nodiscard]] RGTextureDesc ldr_desc(const char* name) {
    RGTextureDesc d{};
    d.extent = {kW, kH};
    d.format = kLdrFormat;
    d.debug_name = name;
    return d;
}

// Fraction of pixels in a box that differ from the flat background.
[[nodiscard]] double covered(const std::vector<std::uint8_t>& px,
                             std::uint32_t x0,
                             std::uint32_t y0,
                             std::uint32_t x1,
                             std::uint32_t y1,
                             std::uint8_t bg) {
    std::size_t hit = 0;
    std::size_t total = 0;
    for (std::uint32_t y = y0; y < y1 && y < kH; ++y) {
        for (std::uint32_t x = x0; x < x1 && x < kW; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 4u;
            ++total;
            if (px[i] != bg || px[i + 1] != bg || px[i + 2] != bg) {
                ++hit;
            }
        }
    }
    return total == 0 ? 0.0 : static_cast<double>(hit) / static_cast<double>(total);
}

} // namespace

TEST_CASE("m13.3b: the HUD draws text, tells glyphs apart, and leaves the frame alone") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the HUD proof");
        return;
    }

    text::HudRenderer hud(*device, kLdrFormat);
    REQUIRE(hud.valid());
    REQUIRE(hud.atlas().valid());

    // A mid-grey background: bright enough that dark text shows, dark enough that light text does.
    constexpr float kBg = 0.25f;

    const auto run = [&](bool draw_hud) {
        RenderGraph graph(*device);
        const RGTexture target = graph.create_texture(ldr_desc("hud-target"));
        fill(graph, target, kBg, kBg, kBg);
        if (draw_hud) {
            hud.begin({kW, kH});
            hud.text(20.0f, 20.0f, "MMMM", text::style::kText, 24.0f);
            hud.text(20.0f, 60.0f, "....", text::style::kText, 24.0f);
            hud.declare(graph, target);
        }
        graph.export_texture(target);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(target), kW, kH, 4);
    };

    const std::vector<std::uint8_t> bare = run(false);
    const std::vector<std::uint8_t> drawn = run(true);
    REQUIRE(bare.size() == drawn.size());

    const std::uint8_t bg_byte = bare[0];
    // Non-vacuity on the control: the background really is flat, so "differs from bg" is
    // meaningful.
    CHECK(covered(bare, 0, 0, kW, kH, bg_byte) == doctest::Approx(0.0));

    SUBCASE("text actually draws") {
        // 'M' at 24 px: four glyphs from x=20, each advancing 24 * 6/7 ≈ 20.6 px, so the run
        // spans roughly 20..99. The 60-wide box below therefore samples the first three.
        const double m_cov = covered(drawn, 20, 20, 20 + 60, 20 + 24, bg_byte);
        CHECK(m_cov > 0.15);
        CHECK(hud.quad_count() == 8); // 4 'M' + 4 '.', and no quad for anything else
    }

    SUBCASE("a dense glyph covers more than a sparse one") {
        // The claim (1) cannot make: 'M' inks most of its cell, '.' inks a 2x2 corner of it. If the
        // pass drew one blob per character these would be equal.
        const double m_cov = covered(drawn, 20, 20, 20 + 60, 20 + 24, bg_byte);
        const double dot_cov = covered(drawn, 20, 60, 20 + 60, 60 + 24, bg_byte);
        CHECK(m_cov > dot_cov * 2.0);
        CHECK(dot_cov > 0.0); // …but the dots are there
    }

    SUBCASE("everything the HUD did not draw is byte-identical") {
        // The overlay contract. A region well below both lines of text must come back untouched —
        // not close, IDENTICAL, because a blend that leaked (a wrong load op, a depth attachment, a
        // stray full-screen quad) would show up as a uniform tint that no tolerance would catch.
        std::size_t differing = 0;
        for (std::uint32_t y = 100; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 4u;
                for (std::size_t c = 0; c < 4; ++c) {
                    if (bare[i + c] != drawn[i + c]) {
                        ++differing;
                    }
                }
            }
        }
        CHECK(differing == 0);
    }
}

TEST_CASE("m13.3b: a panel blends over the frame rather than replacing it") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        return;
    }

    text::HudRenderer hud(*device, kLdrFormat);
    REQUIRE(hud.valid());

    RenderGraph graph(*device);
    const RGTexture target = graph.create_texture(ldr_desc("hud-panel"));
    fill(graph, target, 0.9f, 0.9f, 0.9f); // bright, so a dark translucent panel is unmistakable
    hud.begin({kW, kH});
    hud.panel(10.0f, 10.0f, 100.0f, 40.0f); // the default style: near-black at 0.72 alpha
    hud.declare(graph, target);
    graph.export_texture(target);
    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);
    const std::vector<std::uint8_t> px = read_texture(*device, graph.physical(target), kW, kH, 4);

    const auto at = [&px](std::uint32_t x, std::uint32_t y) {
        return px[(static_cast<std::size_t>(y) * kW + x) * 4u];
    };

    // Inside the panel: much darker than the background, but NOT black — a panel that replaced
    // instead of blending would read as the panel colour exactly, and the whole point of an 0.72
    // alpha is that the frame still shows through.
    const int inside = at(50, 30);
    const int outside = at(200, 30);
    CHECK(outside > 200);         // background survived
    CHECK(inside < outside - 60); // the panel is clearly darker
    CHECK(inside > 10);           // …and did not simply overwrite

    // The edge is where an off-by-one in the pixel→clip mapping shows up: just outside the panel
    // must be pure background.
    CHECK(at(115, 30) == at(200, 30));
}
