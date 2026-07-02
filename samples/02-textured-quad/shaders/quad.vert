// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vertex shader for Rime's textured quad (M3.5). It places each corner in clip space and passes the
// per-vertex texture coordinate through; the rasterizer interpolates that UV across the quad so the
// fragment shader can look the texture up per pixel.
//
// Compiled to SPIR-V at build time (ADR-0008) and embedded as a C array by rime_add_shaders.
#version 450

// Vertex attributes, matching the RHI VertexLayout in quad_render.hpp:
//   location 0: position (vec2, NDC), location 1: uv (vec2, 0..1).
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;

// Handed to the fragment shader (interpolated per-pixel by the rasterizer).
layout(location = 0) out vec2 frag_uv;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    frag_uv = in_uv;
}
