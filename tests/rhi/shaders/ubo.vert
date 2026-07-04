// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vertex shader for the uniform-buffer proof: the same full-screen-triangle-from-gl_VertexIndex
// trick as pushconst.vert (no vertex buffer), so every framebuffer pixel runs the fragment shader
// and shows whatever the bound UBO slice holds.
#version 450

void main() {
    vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
