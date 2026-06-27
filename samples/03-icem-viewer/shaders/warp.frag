// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the vector-field warp (C3): shade the warped surface by the field magnitude with
// the shared colormap (so the deformation reads as cool→hot), under the same simple studio light as the
// surface. Two-sided diffuse (abs N·L) because the warp leaves the rest normals only approximate.
#version 450

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in float v_t;

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
    vec3 N = normalize(v_normal);
    const vec3 L = normalize(vec3(0.4, 0.85, 0.5));
    float diffuse = abs(dot(N, L)); // two-sided
    float up = 0.5 * N.y + 0.5;
    vec3 ambient = mix(vec3(0.20, 0.18, 0.16), vec3(0.55, 0.60, 0.70), up);

    vec3 base = colormap(v_t);
    out_color = vec4(base * (0.55 * ambient + 0.85 * diffuse), 1.0);
}
