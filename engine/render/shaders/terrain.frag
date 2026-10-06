// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The terrain heightfield pass, fragment stage (m19.3, ADR-0062). One directional light plus an
// ambient term, into the HDR target. m19.3 shaded Lambert with one flat albedo; m19.4 (ADR-0063)
// let a tile blend up to four base colours by a cooked weight map; m19.5 (ADR-0064) shades with the
// SAME Cook-Torrance GGX BRDF as pbr_forward.frag (brdf.glsl) and blends metallic and roughness
// alongside the colour, so terrain can look like metal. A tile without a splat map takes the
// flat-material path (`pc.surface.rgb`, `pc.material`).
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
#extension GL_GOOGLE_include_directive : require

#include "brdf.glsl"

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec2 v_local; // tile-local xz, metres

// m19.4: the splat weights (RGBA8_UNORM, one texel = the four layer weights) and the per-tile
// constants. The push block is not the place for per-layer data, so these live in a small uniform buffer made at
// upload. (m19.5 grew the push block to 160 bytes but the splat constants stay here.) A v1 tile binds a 1x1 dummy texture and flag = 0, so the descriptor layout is identical
// for every tile and there is ONE pipeline.
layout(set = 0, binding = 1) uniform sampler2D splat_weights;
layout(set = 0, binding = 2) uniform Splat {
    vec4 info;     // x = 1 when this tile has a splat map, yz = tile extent in metres (x, z)
    vec4 dims;     // xy = weight map size in texels
    vec4 color[4]; // rgb = base colour, w = metallic per layer; an unused slot repeats layer 0
    vec4 roughness; // x..w = roughness of layer 0..3 (same repeat rule)
} splat;

layout(location = 0) out vec4 out_color;

// Must match terrain.vert's block byte for byte (one block, both stages — see TerrainPush).
layout(push_constant) uniform Pc {
    mat4 view_proj;
    vec4 placement;
    vec4 grid;
    vec4 sun;     // xyz = unit direction the light TRAVELS, w = irradiance (W/m², perpendicular)
    vec4 surface; // rgb = albedo in [0,1], w = ambient irradiance
    vec4 eye;     // xyz = camera world position
    vec4 material; // x = metallic, y = roughness (the flat tile's material)
} pc;

void main() {
    // ── THE SPLAT BLEND, AND WHY IT IS WRITTEN AS DIFFERENCES FROM LAYER 0 ────────────────────
    //
    // The obvious formula is  sum_k w_k * c_k.  The reader guarantees the four u8 weights sum to
    // exactly 255, so in exact arithmetic that is a convex combination. In FLOATS it is not: the
    // four n/255 values do not sum to exactly 1.0, linear filtering adds its own rounding, and a
    // GPU may contract the sum into fused multiply-adds. So with all four colours EQUAL the naive
    // sum is still not bit-equal to that colour — a "blend of identical layers" would shift the
    // picture by a few ULP, and the proof could only say "close".
    //
    //     base = c0 + w1*(c1 - c0) + w2*(c2 - c0) + w3*(c3 - c0)
    //
    // is the same quantity when sum(w) = 1 (substitute w0 = 1 - w1 - w2 - w3), but every term
    // (c_k - c0) is EXACTLY 0 when the layers are equal, so the result is c0 bit for bit on every
    // GPU with any filter. Likewise a texel whose weights 1..3 are 0 yields c0 exactly. That is
    // what lets ADR-0063 section 4 anchor the blend with "bit-identical, no margin". w0 is never
    // read: it is implied.
    //
    // m19.5: metallic and roughness are blended in the SAME difference form, for the SAME reason.
    // The anchor ("four equal layers == the flat tile") would otherwise stop being bit-exact the
    // moment a scalar went through the naive weighted sum, and the anchor is what proves the
    // weight map is not quietly adding or removing light.
    vec3 base = pc.surface.rgb;
    float metallic = pc.material.x;
    float roughness = pc.material.y;
    if (splat.info.x > 0.5) {
        // CORNER-aligned, like the height samples: weight texel (0,0) is centred on the tile
        // origin and texel (wc-1, wr-1) on the far corner. Normalised uv puts texel k's centre at
        // (k + 0.5)/dims, so map local/extent in [0,1] onto [0.5/dims, (dims-0.5)/dims]. A
        // one-texel axis gives (dims-1) = 0, i.e. uv 0.5, the only texel there is.
        const vec2 uv = (v_local / splat.info.yz) * ((splat.dims.xy - 1.0) / splat.dims.xy) +
                        0.5 / splat.dims.xy;
        const vec4 w = texture(splat_weights, uv);
        const vec3 c0 = splat.color[0].rgb;
        base = c0 + w.g * (splat.color[1].rgb - c0) + w.b * (splat.color[2].rgb - c0) +
               w.a * (splat.color[3].rgb - c0);
        const float m0 = splat.color[0].w;
        metallic = m0 + w.g * (splat.color[1].w - m0) + w.b * (splat.color[2].w - m0) +
                   w.a * (splat.color[3].w - m0);
        const float r0 = splat.roughness.x;
        roughness = r0 + w.g * (splat.roughness.y - r0) + w.b * (splat.roughness.z - r0) +
                    w.a * (splat.roughness.w - r0);
    }
    // Same floor and remap as pbr_forward: alpha = roughness^2 (perceptual), and a roughness of 0
    // would make GGX's D a delta function that no finite sun can light, so clamp it.
    roughness = clamp(roughness, 0.045, 1.0);
    const float alpha = roughness * roughness;
    const vec3 v = normalize(pc.eye.xyz - v_world);

    vec3 n = normalize(cross(dFdx(v_world), dFdy(v_world)));
    if (n.y < 0.0) {
        n = -n; // see the header: a heightfield's surface normal is never downward
    }

    // `sun.xyz` is the direction the light travels, so the vector TOWARD the light is its negation
    // — the same convention `DirectionalLight` extraction pins in components.hpp. shade_light
    // returns BRDF x irradiance x n.l: for a metallic-0 surface its diffuse is kd*albedo/pi with
    // kd = 1 - Fresnel (the energy the specular lobe took), i.e. Lambert's albedo/pi scaled down by
    // the Fresnel reflectance, plus the specular lobe Lambert never had.
    vec3 radiance =
        shade_light(n, v, -pc.sun.xyz, vec3(pc.sun.w), base, metallic, alpha);

    // AMBIENT is a uniform-environment STAND-IN until terrain reads the sky SH (deferred, ADR-0064).
    // A uniform white environment of irradiance E reflects diffuse (1-metallic)*base*E and a
    // specular term of about f0*E, with f0 = 0.04 for dielectrics and the base colour for metals.
    // Why a flat-ambient metal still reads DARKER than a real one: a metal has no diffuse, so its
    // only ambient light is this f0 term, and a real metal would also mirror the sky and horizon
    // (bright, directional, coloured) — which needs the environment reflection this stand-in lacks.
    const vec3 f0 = mix(vec3(0.04), base, metallic);
    radiance += pc.surface.w * ((1.0 - metallic) * base + f0);
    out_color = vec4(radiance, 1.0);
}
