// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vertex shader for the push-constant proof. It needs no vertex buffer: it emits a single oversized
// triangle from gl_VertexIndex that covers the whole framebuffer, so the fragment shader's per-draw
// push-constant color fills every pixel. (The classic "full-screen triangle" trick.)
#version 450

void main() {
    vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
