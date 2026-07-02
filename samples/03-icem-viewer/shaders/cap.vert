// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Cross-section cap quad (B2b). With no vertex buffer, it generates the two triangles of a rectangle
// lying *on* the cutting plane and spanning the part, from the push constant: cap_rect holds the two
// in-plane axes' extents, cap_meta holds the plane offset and which axis is the plane normal. The
// stencil test (set up by the marking pass) then keeps only the fragments inside the solid; the world
// position is forwarded so cap.frag can colour the cut face by the field.
#version 450

layout(location = 0) out vec3 v_world_pos;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 field_scale; // xyz = world->uvw scale, w = vmin
    vec4 field_bias;  // xyz = world->uvw bias,  w = vmax
    vec4 cap_rect;    // u_lo, u_hi, v_lo, v_hi  (extents of the two in-plane axes)
    vec4 cap_meta;    // x = plane offset, y = axis (0=x, 1=y, 2=z)
} pc;

vec3 place(float u, float v, float off, int axis) {
    if (axis == 0) return vec3(off, u, v);
    if (axis == 1) return vec3(u, off, v);
    return vec3(u, v, off);
}

void main() {
    vec2 uv[4] = vec2[](vec2(pc.cap_rect.x, pc.cap_rect.z), vec2(pc.cap_rect.y, pc.cap_rect.z),
                        vec2(pc.cap_rect.y, pc.cap_rect.w), vec2(pc.cap_rect.x, pc.cap_rect.w));
    int idx[6] = int[](0, 1, 2, 0, 2, 3);
    vec2 c = uv[idx[gl_VertexIndex]];
    vec3 world = place(c.x, c.y, pc.cap_meta.x, int(pc.cap_meta.y + 0.5));
    v_world_pos = world;
    gl_Position = pc.mvp * vec4(world, 1.0);
}
