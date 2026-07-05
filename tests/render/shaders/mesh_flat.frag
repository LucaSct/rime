// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the scene-layer proof: the pushed flat color. Real shading is M5.6's PBR;
// this proof is about geometry, camera, and registry plumbing being right.
#version 450

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
} pc;

layout(location = 0) in vec3 v_normal;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = pc.color;
}
