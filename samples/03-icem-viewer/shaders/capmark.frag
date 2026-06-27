// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Stencil-marking fragment shader for the cross-section cap (B2b). Paired with mesh.vert, it renders
// the part with the same clip-plane discard as mesh.frag, but writes no colour (the pipeline masks it)
// and no depth — it only flips the stencil parity bit per fragment. After this pass, a pixel's stencil
// LSB is the parity of how many kept surfaces lie along its view ray; odd ⇒ the cut plane is inside the
// solid there. Parity (rather than front/back counting) is winding-independent, which matters for STL
// "soup" meshes whose triangle winding isn't guaranteed. See docs/math/clip-cap.md.
#version 450

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal; // unused (kept so the vertex layout matches mesh.vert)

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 cam_pos;
    vec4 clip_plane; // xyz = unit normal, w = signed offset; discard the cut-away half (dot(N,p) > w)
    vec4 field_scale;
    vec4 field_bias;
} pc;

void main() {
    if (dot(pc.clip_plane.xyz, v_world_pos) > pc.clip_plane.w) discard;
    out_color = vec4(1.0); // masked off by the pipeline; the stencil op (Invert) does the real work
}
