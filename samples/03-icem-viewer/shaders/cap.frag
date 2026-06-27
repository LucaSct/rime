// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Cross-section cap fill (B2b). The stencil test has already restricted this draw to the solid part of
// the cutting plane; here we just colour the cut face. With a field bound it is the field sampled on
// the plane — the flat cross-section *slice* of the simulation (the same volume + colormap as mesh.frag,
// deferred from C1). With no field it is a flat "machined metal" grey, a touch darker than the lit
// surface so the cut reads as solid. Unlit on purpose, so the field colour on the slice reads true.
#version 450

layout(location = 0) in vec3 v_world_pos;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler3D u_field;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 field_scale; // xyz = world->uvw scale, w = vmin
    vec4 field_bias;  // xyz = world->uvw bias,  w = vmax  (field on iff vmax > vmin)
    vec4 cap_rect;
    vec4 cap_meta;
} pc;

vec3 colormap(float t) {
    const vec3 c0 = vec3(0.20, 0.30, 0.80);
    const vec3 c1 = vec3(0.10, 0.70, 0.90);
    const vec3 c2 = vec3(0.25, 0.80, 0.30);
    const vec3 c3 = vec3(0.95, 0.85, 0.20);
    const vec3 c4 = vec3(0.90, 0.20, 0.15);
    float x = clamp(t, 0.0, 1.0) * 4.0;
    if (x < 1.0) return mix(c0, c1, x);
    if (x < 2.0) return mix(c1, c2, x - 1.0);
    if (x < 3.0) return mix(c2, c3, x - 2.0);
    return mix(c3, c4, x - 3.0);
}

void main() {
    bool field_on = pc.field_bias.w > pc.field_scale.w;
    vec3 base = vec3(0.62, 0.62, 0.64); // machined-metal grey
    if (field_on) {
        vec3 uvw = v_world_pos * pc.field_scale.xyz + pc.field_bias.xyz;
        float value = texture(u_field, uvw).r;
        float t = (value - pc.field_scale.w) / (pc.field_bias.w - pc.field_scale.w);
        base = colormap(t);
    }
    out_color = vec4(base, 1.0);
}
