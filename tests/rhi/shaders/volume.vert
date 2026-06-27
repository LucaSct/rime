// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Full-screen triangle for the 3-D-texture proof (ADR-0013). No vertex buffer: the three positions
// are generated from gl_VertexIndex (the standard "big triangle" that covers the framebuffer), and
// v_uv runs 0→1 across the screen. Vulkan clip-space Y points down, so v_uv.y = 0 is the top row.
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)); // (0,0),(2,0),(0,2)
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
