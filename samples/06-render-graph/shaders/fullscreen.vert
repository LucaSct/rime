// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fullscreen-triangle vertex shader for 06-render-graph's demo post pass: one oversized triangle
// from gl_VertexIndex, no vertex buffer (the same idiom the engine's own tonemap pass uses). The
// fragment shader locates itself with gl_FragCoord, so no uv varying is needed.
#version 450

void main() {
    vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
