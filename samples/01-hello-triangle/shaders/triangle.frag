// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The fragment shader: it runs once per rasterized pixel and decides that pixel's color. Here it
// simply outputs the interpolated vertex color (opaque). This is the other half of the minimal
// programmable pipeline — vertex shader places the triangle, fragment shader fills it.
#version 450

layout(location = 0) in vec3 frag_color;  // interpolated from the vertex shader's output
layout(location = 0) out vec4 out_color;  // -> the single color attachment (location 0)

void main() {
    out_color = vec4(frag_color, 1.0);
}
