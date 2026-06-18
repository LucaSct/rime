// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The vertex shader for Rime's first triangle. It does the one job a vertex shader must: turn each
// input vertex into a clip-space position (gl_Position). We also pass the per-vertex color through
// to the fragment shader, which the rasterizer interpolates across the triangle for free.
//
// Compiled to SPIR-V at build time (ADR-0008) and embedded as a C array by rime_add_shaders.
#version 450

// Vertex attributes, matching the RHI VertexLayout in triangle_render.hpp:
//   location 0: position (vec2, NDC), location 1: color (vec3, RGB).
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec3 in_color;

// Handed to the fragment shader (interpolated per-pixel by the rasterizer).
layout(location = 0) out vec3 frag_color;

void main() {
    // z = 0 (on the near..far range), w = 1 (no perspective divide for this flat 2D triangle).
    gl_Position = vec4(in_position, 0.0, 1.0);
    frag_color = in_color;
}
