// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Streamline lines (D·V): transform a precomputed streamline vertex (world position + a normalized
// speed) by the camera matrix and forward the speed for the colormap. The lines are a LineList built on
// the CPU by RK4-integrating the computed 3-D velocity field. Push block matches MeshPush (only mvp used).
#version 450

layout(location = 0) in vec4 in_pv; // xyz = world position, w = |u|/vmag_max (the colormap coordinate)

layout(location = 0) out float v_speed;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 cam_pos;
    vec4 clip_plane;
    vec4 field_scale;
    vec4 field_bias;
} pc;

void main() {
    v_speed = in_pv.w;
    gl_Position = pc.mvp * vec4(in_pv.xyz, 1.0);
}
