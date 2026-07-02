// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fills the legend bar with the same 5-stop "turbo-lite" transfer function the mesh colormap uses, so
// the bar is a true key for the surface colours. (The numeric min/max labels need text — Rime's
// from-scratch UI, brick E2 — so for now the range is printed to the console / window title.)
#version 450

layout(location = 0) in float v_t;
layout(location = 0) out vec4 out_color;

vec3 colormap(float t) {
    const vec3 c0 = vec3(0.20, 0.30, 0.80); // 0.00 blue
    const vec3 c1 = vec3(0.10, 0.70, 0.90); // 0.25 cyan
    const vec3 c2 = vec3(0.25, 0.80, 0.30); // 0.50 green
    const vec3 c3 = vec3(0.95, 0.85, 0.20); // 0.75 yellow
    const vec3 c4 = vec3(0.90, 0.20, 0.15); // 1.00 red
    float x = clamp(t, 0.0, 1.0) * 4.0;
    if (x < 1.0) return mix(c0, c1, x);
    if (x < 2.0) return mix(c1, c2, x - 1.0);
    if (x < 3.0) return mix(c2, c3, x - 2.0);
    return mix(c3, c4, x - 3.0);
}

void main() {
    out_color = vec4(colormap(v_t), 1.0);
}
