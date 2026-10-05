// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The terrain heightfield pass, fragment stage (m19.3, ADR-0062). Deliberately the smallest honest
// shading of a terrain: one directional light, Lambert, plus an ambient term, into the HDR target.
// m19.3 drew the heightfield with one flat albedo; m19.4 (ADR-0063) lets a tile blend up to four
// base colours by a cooked weight map. A tile without a splat map still takes the flat-albedo path
// (`pc.surface.rgb`), so the m19.3 picture is untouched.
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
layout(location = 1) in vec2 v_local; // tile-local xz, metres

// m19.4: the splat weights (RGBA8_UNORM, one texel = the four layer weights) and the per-tile
// constants. The 128-byte push block is full, so these live in a small uniform buffer made at
// upload. A v1 tile binds a 1x1 dummy texture and flag = 0, so the descriptor layout is identical
// for every tile and there is ONE pipeline.
layout(set = 0, binding = 1) uniform sampler2D splat_weights;
layout(set = 0, binding = 2) uniform Splat {
    vec4 info;     // x = 1 when this tile has a splat map, yz = tile extent in metres (x, z)
    vec4 dims;     // xy = weight map size in texels
    vec4 color[4]; // base colour per layer; an unused slot repeats layer 0 (a zero difference)
} splat;

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
    // read: it is implied. Only base colour is blended — this shader is Lambert (ADR-0062), so a
    // blended roughness or metallic would be data nothing here reads.
    vec3 base = pc.surface.rgb;
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
    }

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
    const vec3 radiance = base *
                          (pc.sun.w * n_dot_l * 0.31830988618 + pc.surface.w);
    out_color = vec4(radiance, 1.0);
}
