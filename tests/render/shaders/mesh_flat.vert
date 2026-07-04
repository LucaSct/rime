// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vertex shader for the scene-layer proof: transform a MeshRegistry vertex (the locations 0/1/2
// contract — position, normal, uv) by a pushed MVP. The flat-color fragment shader ignores the
// normal/uv here; they exist so this pipeline validates the full registry vertex layout.
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
} pc;

layout(location = 0) out vec3 v_normal;

void main() {
    v_normal = in_normal; // unused by the flat proof; keeps the attribute live
    gl_Position = pc.mvp * vec4(in_position, 1.0);
}
