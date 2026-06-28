// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The provenance panel (E3): lay out ICEM's `.icejson` Ledger on the from-scratch UI so a computed value
// shows *why it is what it is*. Each ledger node is a clickable row — an origin tag (which kind of
// knowledge produced it: a law, a material, a safety factor, a check…) + "label = value unit"; clicking a
// row expands its **derivation** in place: the rows it was computed from (its causal inputs). The list
// scrolls (the caller feeds a scroll offset from the wheel); a fixed header carries the design, the pass/
// fail verdict and the ledger hash. Pure layout over ui.hpp + provenance.hpp, so the render test drives
// the exact same code. See docs/math/provenance-panel.md.
#pragma once

#include <cstdio>
#include <string>

#include "provenance.hpp"
#include "ui.hpp"

namespace rime::viewer {

namespace detail {

// A short tag for each Origin (the vocabulary of the logic of engineering).
[[nodiscard]] inline const char* origin_tag(const std::string& o) {
    if (o == "Input") return "INP";
    if (o == "Law") return "LAW";
    if (o == "Material") return "MAT";
    if (o == "Rule") return "CHK"; // a Rule node is a design check (margin >= 1)
    if (o == "Heuristic") return "HEU";
    if (o == "SimResult") return "SIM";
    if (o == "SafetyFactor") return "SF";
    if (o == "Derived") return "DRV";
    return "?";
}

// A colour per origin, so the kinds of knowledge read apart at a glance.
[[nodiscard]] inline ui::Color origin_color(const std::string& o) {
    if (o == "Input") return {0.45f, 0.62f, 0.90f, 1.0f};        // blue: a given
    if (o == "Law") return {0.92f, 0.80f, 0.35f, 1.0f};          // amber: physics
    if (o == "Material") return {0.35f, 0.80f, 0.78f, 1.0f};     // teal: a property
    if (o == "Rule") return {0.45f, 0.82f, 0.50f, 1.0f};         // green: a check
    if (o == "Heuristic") return {0.72f, 0.55f, 0.88f, 1.0f};    // purple: experience
    if (o == "SimResult") return {0.92f, 0.58f, 0.32f, 1.0f};    // orange: a simulation
    if (o == "SafetyFactor") return {0.88f, 0.45f, 0.40f, 1.0f}; // red: a margin applied
    return {0.78f, 0.80f, 0.85f, 1.0f};                          // Derived: neutral
}

[[nodiscard]] inline std::string fmt_value(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

// "label = value unit" for a node (the font upper-cases it).
[[nodiscard]] inline std::string node_text(const ProvNode& n) {
    return n.label + " = " + fmt_value(n.value) + (n.unit.empty() || n.unit == "-" ? "" : " " + n.unit);
}

} // namespace detail

// Build the provenance panel into `gui` for a full-window view of width W, height H. `selected` is the
// row whose derivation is expanded (-1 = none) and is updated on click; `scroll` shifts the list.
// Returns the total content height (the caller clamps scroll to it).
inline float build_provenance_panel(ui::Ui& gui, const Provenance& prov, int& selected, float scroll,
                                    float W, float H) {
    using ui::Color;
    constexpr float ts = 1.6f;       // body text scale
    const float row_h = ui::kGlyphH * ts + 8.0f;
    const float pad = 14.0f;
    const float head_h = 86.0f;       // fixed header band
    const float text_x = pad + 6.0f + ui::Ui::text_width("CHK ", ts); // body after the origin tag

    gui.panel(0.0f, 0.0f, W, H, Color{0.08f, 0.09f, 0.12f, 1.0f}); // background fill

    // The scrolling list, drawn first so the opaque header band below can cover rows scrolled past the
    // top. A row is emitted only while on-screen (cheap clipping without a scissor).
    const float list_top = head_h;
    float y = list_top - scroll;
    float content = 0.0f;
    const auto row_visible = [&](float ry) { return ry + row_h > list_top && ry < H; };

    for (std::size_t i = 0; i < prov.nodes.size(); ++i) {
        const ProvNode& n = prov.nodes[i];
        const bool sel = selected == static_cast<int>(i);
        if (row_visible(y)) {
            const bool hov = gui.hovered_in(pad, y, W - 2.0f * pad, row_h);
            if (gui.clicked_in(pad, y, W - 2.0f * pad, row_h))
                selected = sel ? -1 : static_cast<int>(i);
            gui.quad(pad, y, W - 2.0f * pad, row_h - 2.0f,
                     sel ? Color{0.20f, 0.28f, 0.40f, 1.0f}
                         : (hov ? Color{0.15f, 0.17f, 0.22f, 1.0f} : Color{0.10f, 0.11f, 0.14f, 1.0f}));
            gui.text(pad + 6.0f, y + 4.0f, detail::origin_tag(n.origin), ts, detail::origin_color(n.origin));
            gui.text(text_x, y + 4.0f, detail::node_text(n), ts, Color{0.86f, 0.88f, 0.92f, 1.0f});
        }
        y += row_h;
        content += row_h;

        if (sel) { // expand the derivation: the rows this value was computed from
            const float sub = row_h * 0.85f;
            const auto sub_row = [&](const std::string& s) {
                if (row_visible(y))
                    gui.text(pad + 44.0f, y + 3.0f, s, ts * 0.92f, Color{0.60f, 0.66f, 0.76f, 1.0f});
                y += sub;
                content += sub;
            };
            if (n.inputs.empty()) {
                sub_row("<- given (a specification input)");
            } else {
                for (std::uint32_t cid : n.inputs)
                    if (cid < prov.nodes.size()) sub_row("<- " + detail::node_text(prov.nodes[cid]));
            }
        }
    }

    // The header band on top, then the title / verdict / hash / hint.
    gui.quad(0.0f, 0.0f, W, head_h, Color{0.13f, 0.15f, 0.20f, 1.0f});
    gui.text(pad, 10.0f, "PROVENANCE: " + prov.design, 2.4f, Color{0.95f, 0.96f, 1.0f, 1.0f});
    gui.text(pad, 40.0f, std::string("VERDICT: ") + (prov.passes ? "PASS" : "FAIL"), 2.0f,
             prov.passes ? Color{0.45f, 0.82f, 0.50f, 1.0f} : Color{0.88f, 0.42f, 0.36f, 1.0f});
    gui.text(pad + 230.0f, 44.0f, "HASH " + prov.hash, 1.5f, Color{0.50f, 0.55f, 0.62f, 1.0f});
    gui.text(pad, 66.0f, "CLICK A VALUE TO SEE WHY - WHEEL SCROLLS - ESC QUITS", 1.4f,
             Color{0.50f, 0.55f, 0.62f, 1.0f});
    return content;
}

} // namespace rime::viewer
