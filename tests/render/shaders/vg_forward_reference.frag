// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Forward reference, fragment stage: the same three outputs as vg_resolve.frag, computed the
// ordinary way — hardware-interpolated UV and the rasterizer's fine screen-space derivatives.
// The albedo is sampled with those fine derivatives made explicit rather than with implicit
// texture(): whether texture() uses fine or per-quad (coarse) derivatives is implementation-
// defined, and a reference that changes definition per driver cannot prove anything.
#version 450

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D albedo;

layout(push_constant) uniform Pc {
    mat4 mvp;
    uint material;
} pc;

layout(location = 0) out uint out_material;
layout(location = 1) out vec4 out_uv;
layout(location = 2) out vec4 out_albedo;

void main() {
    out_material = pc.material;
    vec2 ddx = dFdxFine(in_uv);
    vec2 ddy = dFdyFine(in_uv);
    out_uv = vec4(in_uv, ddx.x, ddy.y);
    out_albedo = textureGrad(albedo, in_uv, ddx, ddy);
}
