// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine's fullscreen-pass vertex shader (M5.6): one oversized triangle from gl_VertexIndex,
// no vertex buffer — the classic trick (same idiom as the rhi tests' pushconst.vert). A single
// triangle beats a two-triangle quad for post passes: no diagonal seam of duplicated fragment
// work, one primitive to set up. Fragment shaders locate themselves with gl_FragCoord (exact,
// orientation-proof), so no uv varying is emitted.
#version 450

void main() {
    vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
