// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The terrain heightfield pass, fragment stage (m19.3, ADR-0062). Deliberately the smallest honest
// shading of a terrain: one directional light, Lambert, plus an ambient term, into the HDR target.
// Splat blending (several materials weighted by a cooked weight map) is a later M19 brick and this
// shader is not where that decision gets pre-empted — it carries a single flat albedo so that the
// brick's claim is "the heightfield draws", not "terrain looks right".
//
// ── THE NORMAL COMES FROM THE GEOMETRY, NOT FROM A SECOND HEIGHT READ ────────────────────────
//
// The tempting alternative is to fetch the four neighbouring samples in the vertex stage and take
// a central difference. That produces a SMOOTH normal field, which looks nicer — and which does
// not describe the surface anyone collides with: `rime::physics` collides against flat triangles
// and reports their faceted plane normals (ADR-0060 §2, "forcing the triangle normal is the
// point"). So the normal here is the drawn triangle's own plane normal, recovered from the
// interpolated world position:
//
//   over a triangle, world position is an affine function of the screen position, so dFdx and
//   dFdy of it are two independent vectors lying IN the triangle's plane, and their cross product
//   is that plane's normal — exactly, not approximately.
//
// This is the standard "derivative normal" trick. It costs no extra texture reads, it cannot
// disagree with the geometry because it is derived from the geometry, and it makes the shading
// faceted in precisely the way the collision is faceted. The sign of a screen-space cross product
// depends on the winding the triangle happens to present, so it is resolved against the one thing
// a heightfield always knows: its up axis is local +Y, and the surface is a function of XZ, so a
// terrain normal can never point downward.
#version 450

layout(location = 0) in vec3 v_world;

layout(location = 0) out vec4 out_color;

// Must match terrain.vert's block byte for byte (one block, both stages — see TerrainPush).
layout(push_constant) uniform Pc {
    mat4 view_proj;
    vec4 placement;
    vec4 grid;
    vec4 sun;     // xyz = unit direction the light TRAVELS, w = irradiance (W/m², perpendicular)
    vec4 surface; // rgb = albedo in [0,1], w = ambient irradiance
} pc;

void main() {
    vec3 n = normalize(cross(dFdx(v_world), dFdy(v_world)));
    if (n.y < 0.0) {
        n = -n; // see the header: a heightfield's surface normal is never downward
    }

    // `sun.xyz` is the direction the light travels, so the vector TOWARD the light is its negation
    // — the same convention `DirectionalLight` extraction pins in components.hpp.
    const float n_dot_l = max(dot(n, -pc.sun.xyz), 0.0);

    // Lambert: outgoing radiance = albedo/π × irradiance. The 1/π is the normalisation that makes
    // a white Lambertian surface reflect exactly the energy it receives and no more; dropping it
    // is the single most common way a renderer ends up π times too bright.
    const vec3 radiance = pc.surface.rgb *
                          (pc.sun.w * n_dot_l * 0.31830988618 + pc.surface.w);
    out_color = vec4(radiance, 1.0);
}
