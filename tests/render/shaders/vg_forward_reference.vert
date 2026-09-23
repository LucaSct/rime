// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The CONVENTIONAL half of the M18 step-2 parity proof: a plain forward draw of the same cooked
// vertices (vertex attributes, index buffer, hardware interpolation) that the visibility-buffer
// resolve must reproduce. `invariant` pins gl_Position so the two paths rasterize the same
// coverage from the same matrix * position product (vg_visibility.vert declares it too).
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

layout(push_constant) uniform Pc {
    mat4 mvp;
    uint material; // material_slot + 1, written verbatim
} pc;

layout(location = 0) out vec2 out_uv;

invariant gl_Position;

void main() {
    out_uv = in_uv;
    gl_Position = pc.mvp * vec4(in_position, 1.0);
}
