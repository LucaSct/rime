// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The gas-path chart (D4): a from-scratch 2-D line plot of a computed scalar field *along the flow axis*,
// overlaid on the 3-D view. ICEM's compressible nozzle (brick26) colours its surface by Mach; this turns
// the same field into the engineer's gas-path diagram — Mach (or temperature, or pressure) versus axial
// station, so you read the flow accelerate through the throat and cross M = 1 into the supersonic branch.
// Pure layout over ui.hpp (which gained a line() primitive for the curve/axes), so the render test drives
// the exact same code. See docs/math/gas-path-chart.md.
#pragma once

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "field.hpp"
#include "ui.hpp"

namespace rime::viewer {

// Sample the scalar field down its **flow axis** — taken to be the longest grid axis, which is the flow
// direction for ICEM's revolved gas-path parts (a nozzle is long and slender). Each entry is the mean of
// the valid voxels in that cross-section slice, i.e. the cross-sectional-average profile; a slice with no
// valid voxels is NaN. Pure (no GPU), so the test pins it directly.
[[nodiscard]] inline std::vector<float> sample_axial_profile(const ScalarField& f,
                                                             int* axis_out = nullptr) {
    const std::uint32_t n[3] = {f.nx, f.ny, f.nz};
    const int axis = (n[2] >= n[0] && n[2] >= n[1]) ? 2 : (n[1] >= n[0] ? 1 : 0);
    if (axis_out)
        *axis_out = axis;
    if (n[axis] == 0 || f.rgba.size() < static_cast<std::size_t>(n[0]) * n[1] * n[2] * 4)
        return {};

    std::vector<float> prof(n[axis]);
    const std::uint32_t u_n = n[(axis + 1) % 3], v_n = n[(axis + 2) % 3];
    for (std::uint32_t a = 0; a < n[axis]; ++a) {
        double sum = 0.0;
        std::uint32_t cnt = 0;
        for (std::uint32_t u = 0; u < u_n; ++u) {
            for (std::uint32_t v = 0; v < v_n; ++v) {
                std::uint32_t ijk[3];
                ijk[axis] = a;
                ijk[(axis + 1) % 3] = u;
                ijk[(axis + 2) % 3] = v;
                const std::size_t g = static_cast<std::size_t>(ijk[0]) +
                                      f.nx * (static_cast<std::size_t>(ijk[1]) +
                                              static_cast<std::size_t>(f.ny) * ijk[2]);
                if (f.rgba[g * 4 + 1] > 0.5f) { // G channel = validity
                    sum += f.rgba[g * 4 + 0];   // R channel = value
                    ++cnt;
                }
            }
        }
        prof[a] = cnt ? static_cast<float>(sum / cnt) : std::numeric_limits<float>::quiet_NaN();
    }
    return prof;
}

namespace detail {
[[nodiscard]] inline std::string chart_num(float v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.3g", static_cast<double>(v));
    return buf;
}
} // namespace detail

// Draw the gas-path chart into `gui` within the rect (x,y,w,h): a framed line plot of the field's axial
// profile (inlet on the left, exit on the right), the y-axis spanning the field's value range, with a
// dashed M = 1 reference line when the field is "mach" and the range crosses sonic. Pure layout.
inline void build_gas_path_chart(ui::Ui& gui, const ScalarField& field, float x, float y, float w,
                                 float h) {
    using ui::Color;
    const std::vector<float> prof = sample_axial_profile(field);

    const Color frame{0.50f, 0.55f, 0.62f, 1.0f};
    const Color curve{0.96f, 0.82f, 0.28f, 1.0f}; // amber, like the LAW origin
    const Color ink{0.88f, 0.90f, 0.94f, 1.0f};
    const Color faint{0.40f, 0.44f, 0.52f, 1.0f};

    gui.quad(x, y, w, h, Color{0.06f, 0.07f, 0.10f, 0.94f}); // panel background
    gui.text(x + 8.0f, y + 6.0f, field.name + " [" + (field.unit.empty() ? "-" : field.unit) + "]",
             1.5f, ink);

    // The plot area, inset for the axis labels.
    const float ml = 52.0f, mr = 14.0f, mt = 30.0f, mb = 22.0f;
    const float px = x + ml, py = y + mt, pw = w - ml - mr, ph = h - mt - mb;
    if (pw <= 2.0f || ph <= 2.0f || prof.size() < 2)
        return;

    const float vlo = field.vmin;
    const float vhi = field.vmax > field.vmin ? field.vmax : field.vmin + 1.0f;
    const auto X = [&](std::size_t i) {
        return px + pw * static_cast<float>(i) / static_cast<float>(prof.size() - 1);
    };
    const auto Y = [&](float v) { return py + ph * (1.0f - (v - vlo) / (vhi - vlo)); };

    // Axes + top/bottom value labels.
    gui.line(px, py, px, py + ph, 1.5f, frame);             // y axis
    gui.line(px, py + ph, px + pw, py + ph, 1.5f, frame);   // x axis (baseline)
    gui.text(x + 6.0f, py - 5.0f, detail::chart_num(vhi), 1.2f, faint);
    gui.text(x + 6.0f, py + ph - 5.0f, detail::chart_num(vlo), 1.2f, faint);
    gui.text(px + 2.0f, py + ph + 6.0f, "INLET", 1.2f, faint);
    gui.text(px + pw - ui::Ui::text_width("EXIT", 1.2f), py + ph + 6.0f, "EXIT", 1.2f, faint);

    // The sonic reference: where the gas crosses M = 1 into the supersonic branch.
    if (field.name == "mach" && vlo < 1.0f && vhi > 1.0f) {
        const float ys = Y(1.0f);
        for (float xx = px; xx < px + pw; xx += 12.0f) // dashed
            gui.line(xx, ys, std::min(xx + 6.0f, px + pw), ys, 1.0f, faint);
        gui.text(px + pw - ui::Ui::text_width("M=1", 1.2f) - 2.0f, ys - 14.0f, "M=1", 1.2f, faint);
    }

    // The profile curve: connect consecutive finite stations.
    for (std::size_t i = 1; i < prof.size(); ++i) {
        if (std::isfinite(prof[i - 1]) && std::isfinite(prof[i]))
            gui.line(X(i - 1), Y(prof[i - 1]), X(i), Y(prof[i]), 2.0f, curve);
    }
}

} // namespace rime::viewer
