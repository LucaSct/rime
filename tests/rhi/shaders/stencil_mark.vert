// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// A centred triangle (no vertex buffer) for the stencil proof: it covers the middle of the frame but
// not the corners, so a following stencil-tested full-screen draw fills only the centre.
#version 450

void main() {
    vec2 p[3] = vec2[](vec2(0.0, -0.7), vec2(0.7, 0.7), vec2(-0.7, 0.7));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
