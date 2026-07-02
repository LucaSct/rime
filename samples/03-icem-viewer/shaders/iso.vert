// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Full-screen triangle for the isosurface / volume raymarch (C2). No vertex buffer; v_uv runs 0..1 over
// the screen and the fragment shader turns it into a world-space view ray via the inverse view-proj.
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
