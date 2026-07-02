// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vertex shader for the depth-test proof. Unlike the flat-2D triangle (which pins z = 0), this passes
// a full vec3 position straight through to clip space, so each triangle carries its own NDC depth —
// that is what the depth buffer compares. Color is interpolated through to the fragment shader.
//
// Compiled to SPIR-V at build time (ADR-0008) and embedded as a C array by rime_add_shaders.
#version 450

layout(location = 0) in vec3 in_position; // NDC x,y and z in [0,1] (near..far)
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 frag_color;

void main() {
    // w = 1 → no perspective divide; gl_Position.z / w is the NDC depth written to the depth buffer.
    gl_Position = vec4(in_position, 1.0);
    frag_color = in_color;
}
