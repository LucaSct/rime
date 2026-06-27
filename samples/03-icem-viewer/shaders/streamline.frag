// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Colour a streamline by its (normalized) flow speed with the shared colormap — cool where the flow is
// slow, hot where it accelerates (e.g. through a contraction). See docs/math/streamlines.md.
#version 450

layout(location = 0) in float v_speed;
layout(location = 0) out vec4 out_color;

vec3 colormap(float t) {
    const vec3 c0 = vec3(0.20, 0.30, 0.80);
    const vec3 c1 = vec3(0.10, 0.70, 0.90);
    const vec3 c2 = vec3(0.25, 0.80, 0.30);
    const vec3 c3 = vec3(0.95, 0.85, 0.20);
    const vec3 c4 = vec3(0.90, 0.20, 0.15);
    float x = clamp(t, 0.0, 1.0) * 4.0;
    if (x < 1.0) return mix(c0, c1, x);
    if (x < 2.0) return mix(c1, c2, x - 1.0);
    if (x < 3.0) return mix(c2, c3, x - 2.0);
    return mix(c3, c4, x - 3.0);
}

void main() {
    out_color = vec4(colormap(v_speed), 1.0);
}
