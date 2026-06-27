// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vector-field warp (C3). The vertex fetches the vec3 field (a displacement or a modal mode shape) at
// its rest position from the 3-D field volume, and displaces the vertex along it by an animated gain.
// This is a vertex texture fetch — the field volume's descriptor is visible to the vertex stage. The
// normalized magnitude rides to the fragment shader for the colormap. See docs/math/colormap.md.
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out float v_t; // |displacement| / vmag_max, the colormap coordinate

layout(set = 0, binding = 0) uniform sampler3D u_field; // xyz = vector, w = validity

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 cam_pos;
    vec4 clip_plane;
    vec4 field_scale; // xyz = world->uvw scale, w = warp gain (animated displacement multiplier)
    vec4 field_bias;  // xyz = world->uvw bias,  w = vmag_max (magnitude normalization)
} pc;

void main() {
    vec3 uvw = in_position * pc.field_scale.xyz + pc.field_bias.xyz;
    vec3 disp = texture(u_field, uvw).xyz;
    vec3 warped = in_position + disp * pc.field_scale.w; // animated warp
    v_world_pos = warped;
    v_normal = in_normal; // rest normal (approximate after warp — fine for a deformation preview)
    v_t = length(disp) / max(pc.field_bias.w, 1e-12);
    gl_Position = pc.mvp * vec4(warped, 1.0);
}
