// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The field-colormap legend: a vertical colour-scale bar on the right edge of the screen, drawn as a
// 4-vertex triangle strip with no vertex buffer (positions from gl_VertexIndex). v_t runs 1 at the top
// (hot) to 0 at the bottom (cold) — the colormap domain. Vulkan clip-space Y points down, so screen
// top is y = -1; the bar is placed in NDC directly.
#version 450

layout(location = 0) out float v_t;

void main() {
    int ix = gl_VertexIndex & 1;  // 0,1,0,1 → left/right edge of the bar
    int iy = gl_VertexIndex >> 1; // 0,0,1,1 → top/bottom of the bar
    float x = (ix == 0) ? 0.84 : 0.90;        // bar spans x ∈ [0.84, 0.90]
    float y = mix(-0.6, 0.6, float(iy));        // iy=0 top (y=-0.6), iy=1 bottom (y=+0.6)
    v_t = mix(1.0, 0.0, float(iy));             // top = 1 (hot), bottom = 0 (cold)
    gl_Position = vec4(x, y, 0.0, 1.0);
}
