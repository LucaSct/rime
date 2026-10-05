// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The terrain heightfield pass, fragment stage (m19.3, ADR-0062). One directional light plus an
// ambient term, into the HDR target. m19.3 shaded Lambert with one flat albedo; m19.4 (ADR-0063)
// let a tile blend up to four base colours by a cooked weight map; m19.5 (ADR-0064) shades with the
// SAME Cook-Torrance GGX BRDF as pbr_forward.frag (brdf.glsl) and blends metallic and roughness
// alongside the colour, so terrain can look like metal. A tile without a splat map takes the
// flat-material path (`pc.surface.rgb`, `pc.material`). m19.6 (ADR-0065) replaces the flat ambient
// with the SKY when the caller binds one: SH irradiance for the diffuse, and the sky-view LUT along
// the mirror direction for the specular — the part that makes a metal look like a metal.
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
#include "sky_mapping.glsl" // skyview_uv_from_direction — the same mapping the LUT was baked with

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

// m19.6: the sky's lighting half (SkyLightBinding). ALWAYS bound — the pass binds its own 1x1 dummy
// LUT and all-zero SH buffer when the caller has no sky — and gated by the SH buffer's own flag
// (sky_sh_enabled()), the contract every sky consumer keeps: "off" is a shader branch, never the
// absence of a resource, so the no-sky draw takes exactly m19.5's instructions.
layout(set = 0, binding = 3) uniform sampler2D skyview_lut;
#define SKY_SH_BINDING 4
#include "sky_sh_eval.glsl"

layout(location = 0) out vec4 out_color;

// ── THE ENVIRONMENT BRDF, ANALYTICALLY ────────────────────────────────────────────────────────
//
// Lighting a surface by a whole environment means integrating the GGX lobe against it. The
// "split-sum" approximation (Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013) factors that
// integral into (the environment averaged over the lobe) x (the BRDF integrated against a WHITE
// environment). The second factor depends only on f0, roughness and n.v, and is linear in f0:
// f0 * A + B. Unreal stores A and B in a 2-D lookup texture; Karis' mobile follow-up ("Physically
// Based Shading on Mobile", 2014) fits them with the handful of terms below — "EnvBRDFApprox". It is
// within a few percent of the LUT, and it costs no texture, no bake and no binding, which is why
// terrain uses it rather than growing a second lookup table. `roughness` is PERCEPTUAL roughness.
vec3 env_brdf_approx(vec3 f0, float roughness, float n_dot_v) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

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

    const vec3 f0 = mix(vec3(0.04), base, metallic);
    if (sky_sh_enabled()) {
        // ── THE SKY (m19.6, ADR-0065) ─────────────────────────────────────────────────────────
        //
        // DIFFUSE. sky_sh_irradiance() returns the radiance leaving a WHITE Lambertian surface
        // facing n — irradiance already divided by pi (sky_sh_eval.glsl), the very units the flat
        // `ambient` stood in for. So it replaces `ambient` one for one, weighted by the same
        // (1 - metallic) * base, and gains no further 1/pi — pbr_forward_shadowed's
        // `albedo * sky_ambient` makes the same call.
        const vec3 sky_diffuse = sky_sh_irradiance(n);
        radiance += (1.0 - metallic) * base * sky_diffuse;

        // SPECULAR. What a mirror shows is the sky along r = reflect(-v, n), read from the LUT.
        //
        // BELOW THE HORIZON the LUT holds the PLANET's ground as seen from altitude, which is not
        // what a terrain reflects: a downward ray from a slope meets more terrain (or whatever
        // stands on it), and nothing here can trace that. Terrain mostly reflects the sky LOW, so
        // the reflection is clamped to the horizon row (uv.y = 0.5 is elevation 0 in the square-
        // root warp): azimuth kept, elevation floored. A grazing view of a slope therefore mirrors
        // the horizon glow rather than a dark planet disc — brighter than the truth in a valley,
        // which is the same known limit as the missing sky occlusion (ADR-0065).
        const vec3 r = reflect(-v, n);
        vec2 uv = skyview_uv_from_direction(r);
        uv.y = max(uv.y, 0.5);
        const vec3 sky_mirror = texture(skyview_lut, uv).rgb;

        // ROUGHNESS. A rough lobe averages the sky over a wide cone; a real engine samples a
        // prefiltered (pre-blurred) mip chain for that. The sky-view LUT has no mips, so it can
        // only answer "the sky in exactly this direction". At the rough end, the lobe is about as
        // wide as the cosine lobe the SH already integrates — and the SH, divided by pi (which
        // sky_sh_irradiance already is), is the cosine-weighted AVERAGE radiance of the sky around
        // n. So the environment fades from the LUT's point sample to the SH's average as
        //     t = alpha = roughness^2,
        // because the GGX lobe's angular width grows like alpha, not like perceptual roughness:
        // roughness 0.1 is still 99% mirror, roughness 0.5 is a 25% blend, 1.0 is all SH. The SH
        // is evaluated at n rather than r, since a fully rough lobe is centred near the normal.
        // This is a stand-in for prefiltered radiance, not a prefilter: mid roughness shows a
        // sharp-ish sky dimmed by a blurred one, rather than one properly blurred sky.
        const vec3 sky_specular = mix(sky_mirror, sky_diffuse, alpha);
        radiance += sky_specular * env_brdf_approx(f0, roughness, max(dot(n, v), 1e-4));
    } else {
        // AMBIENT is a uniform-environment STAND-IN when no sky is bound (m19.5, ADR-0064) —
        // kept instruction for instruction, so a tile without a sky renders bit-identically to
        // m19.5. A uniform white environment of irradiance E reflects diffuse (1-metallic)*base*E
        // and a specular term of about f0*E, with f0 = 0.04 for dielectrics and the base colour
        // for metals. A flat-ambient metal reads DARKER than a real one: a metal has no diffuse,
        // so its only ambient light is this f0 term, where a real metal mirrors the sky — the
        // branch above.
        radiance += pc.surface.w * ((1.0 - metallic) * base + f0);
    }
    out_color = vec4(radiance, 1.0);
}
