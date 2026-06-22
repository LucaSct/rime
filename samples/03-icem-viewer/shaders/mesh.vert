// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vertex shader for the ICEM viewer's lit mesh pass. It transforms the vertex into clip space with the
// per-frame model-view-projection matrix delivered as a push constant (ADR-0012), and forwards the
// world-space position and normal to the fragment shader for lighting. The model transform is identity
// — the viewer orbits the camera, not the part — so the incoming position/normal are already in world
// space, which keeps the push constant down to one matrix (+ the camera position).
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 cam_pos; // xyz = camera/eye position in world space
} pc;

void main() {
    v_world_pos = in_position;
    v_normal = in_normal;
    gl_Position = pc.mvp * vec4(in_position, 1.0);
}
