// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The terrain heightfield pass, fragment stage (m19.3, ADR-0062). One directional light plus an
// ambient term, into the HDR target. m19.3 shaded Lambert with one flat albedo; m19.4 (ADR-0063)
// let a tile blend up to four base colours by a cooked weight map (m19.7c, ADR-0066: the weights
// are redistributed by each layer's height first — see height_blend()); m19.5 (ADR-0064) shades with the
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
// m19.8d3: xy = position in the tile's SAMPLE grid (integers at vertices), z = the vertex stage's
// geomorph factor, interpolated — see terrain.vert.
layout(location = 2) in vec3 v_lod;

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
    // m19.7b: metres per texture repeat along world X, Z — [0] = layer 0 (xy), layer 1 (zw);
    // [1] = layers 2, 3. Appended after m19.5's block, so its first 112 bytes are unchanged.
    vec4 uv_scale[2];
    // m19.7c: height-blend contrast of layer 0..3 (x..w), >= 0; 0 = no redistribution. Appended.
    vec4 height_contrast;
} splat;

// m19.6: the sky's lighting half (SkyLightBinding). ALWAYS bound — the pass binds its own 1x1 dummy
// LUT and all-zero SH buffer when the caller has no sky — and gated by the SH buffer's own flag
// (sky_sh_enabled()), the contract every sky consumer keeps: "off" is a shader branch, never the
// absence of a resource, so the no-sky draw takes exactly m19.5's instructions.
layout(set = 0, binding = 3) uniform sampler2D skyview_lut;
#define SKY_SH_BINDING 4
#include "sky_sh_eval.glsl"

// m19.7b (ADR-0066 addendum): each layer's packed albedo+height texture — RGB = albedo, decoded
// from sRGB by the format; A = height, LINEAR (an sRGB format leaves alpha alone). A layer without
// a texture binds the pass's 1x1 white texel, which samples as exactly (1, 1, 1, 0). Four separate
// bindings rather than one array: layers are independently authored assets of different sizes.
layout(set = 0, binding = 5) uniform sampler2D layer_tex0;
layout(set = 0, binding = 6) uniform sampler2D layer_tex1;
layout(set = 0, binding = 7) uniform sampler2D layer_tex2;
layout(set = 0, binding = 8) uniform sampler2D layer_tex3;

// m19.8d3 (ADR-0072): the APPEARANCE BAKES — a coarse LOD tile's base colour (RGBA8 sRGB) and
// material (R = metallic, G = roughness), one texel per SAMPLE of the tile, cooked offline as the
// average of the level-0 ground beneath. 9/10 are this tile's own (a parent tile shades from them
// instead of from a splat palette it does not have); 11/12 are its PARENT's, which a morphing
// tile fades toward. Always bound (the pass's 1x1 texel when absent) and gated by push-constant
// flags, like the sky: "no bake" is a branch, never a missing resource.
layout(set = 0, binding = 9) uniform sampler2D bake_color;
layout(set = 0, binding = 10) uniform sampler2D bake_material;
layout(set = 0, binding = 11) uniform sampler2D parent_bake_color;
layout(set = 0, binding = 12) uniform sampler2D parent_bake_material;

layout(location = 0) out vec4 out_color;

// lod_tile.z bits (kTerrainPush* in terrain_pass.hpp); bits 0..4 are the vertex stage's.
const int kOwnBake = 32;     // shade from bake_color / bake_material
const int kParentBake = 64;  // fade toward parent_bake_* by the morph factor
const int kQuadrantX = 128;  // this tile is its parent's +x half
const int kQuadrantZ = 256;  // ... +z half

// ── SAMPLING A BAKE: BILINEAR BY HAND, IN SAMPLE SPACE ────────────────────────────────────────
//
// `s` is a position in the bake's texel grid, where texel (i, j) sits AT sample (i, j) — vertex-
// aligned, not centred on cells. The four texels around `s` are fetched by integer index and
// blended here rather than through a LINEAR sampler, for two reasons:
//
//   * EXACT AT A VERTEX. With f = 0 the blend below is (1 - 0) * a + 0 * b = a, bit for bit. A
//     hardware filter computes its weights in fixed point from a normalised coordinate
//     ((i + 0.5) / N, itself rounded), so "the texel at this vertex" comes back to within a
//     weight step of it, on a grid that differs between GPUs (ADR-0066's m19.7b seam bound is
//     exactly that effect). Two tiles that share an edge fetch byte-identical edge texels with a
//     zero cross-edge weight, so they agree on the edge to the bit, not to a filter tolerance.
//   * NO SAMPLER STATE TO GET WRONG. No half-texel offset, no mips, no wrap mode.
//
// texelFetch on an sRGB texture still decodes to linear, so the blend is in linear light.
void bake_sample(sampler2D color, sampler2D material, vec2 s, out vec3 base, out vec2 mr) {
    const ivec2 last = textureSize(color, 0) - ivec2(1);
    const vec2 c = clamp(s, vec2(0.0), vec2(last));
    const vec2 fl = floor(c);
    const vec2 f = c - fl;
    const ivec2 i0 = ivec2(fl);
    const ivec2 i1 = min(i0 + ivec2(1), last);
    const ivec2 i10 = ivec2(i1.x, i0.y);
    const ivec2 i01 = ivec2(i0.x, i1.y);
    base = (1.0 - f.y) * ((1.0 - f.x) * texelFetch(color, i0, 0).rgb +
                          f.x * texelFetch(color, i10, 0).rgb) +
           f.y * ((1.0 - f.x) * texelFetch(color, i01, 0).rgb +
                  f.x * texelFetch(color, i1, 0).rgb);
    mr = (1.0 - f.y) * ((1.0 - f.x) * texelFetch(material, i0, 0).rg +
                        f.x * texelFetch(material, i10, 0).rg) +
         f.y * ((1.0 - f.x) * texelFetch(material, i01, 0).rg +
                f.x * texelFetch(material, i1, 0).rg);
}

// The environment BRDF (Karis' analytic EnvBRDFApprox) this shader weights the sky's specular with
// is env_brdf_approx() in brdf.glsl — moved there, verbatim, in m19.6b so the forward PBR pass
// mirrors the sky through the SAME fit rather than a second copy.

// ── HEIGHT-BASED SPLAT REDISTRIBUTION (m19.7c, ADR-0066 §5; m19.7c fix 1, ADR-0066 addendum) ──
//
// A painted weight map alone cross-fades two layers into one even smudge. Real ground does not do
// that: where grass meets gravel, the gravel shows first in the LOW cracks of the grass and the
// grass holds on to its HIGH tufts. Height blending gets that from data the layers already carry:
// each layer has a height map, and wherever two layers overlap, the one whose surface is locally
// higher takes a larger share of the pixel than the painter gave it.
//
//     e_k = c_k * h_k                        g_k = exp2(e_k - e_ref)
//     b_k = w_k * g_k / sum_j (w_j * g_j)
//
// w = the painted weights, h = the layer heights in [0,1], c = each layer's contrast (>= 0).
// e = c * h is the layer's EFFECTIVE height, in halvings: how far its surface stands above the
// ground. e_ref is the highest e over PAINTED (w > 0) layers. The layer at e_ref keeps g = 1; a
// layer e below it is scaled by 2^(-e). The result is renormalised, so b is again a set of weights.
//
// WHY A COMMON e_ref CANCELS. e_ref multiplies every g_k by the same factor 2^(-e_ref), so it
// drops out of b exactly. Its only job is to keep each exponent <= 0 for a painted layer, which
// keeps every g in (0, 1] and the sum finite. The m19.7c form used a per-layer reference, c_k *
// (h_k - h_max). There the reference is scaled by c_k, so it does NOT cancel across layers of
// different contrast: a tall layer that entered at the faintest painted weight raised h_max and
// re-divided the layers beneath it by their own contrasts, a step at the edge of its region
// (ADR-0066, m19.7c addendum). Taking e_k = c_k * h_k first makes the reference one number for
// every layer, so the others' split no longer depends on which layers are painted elsewhere.
//
// WHAT CONTRAST MEANS NOW. It scales the layer's own height map: a layer of contrast 6 at height
// 0.5 stands as tall (e = 3) as a layer of contrast 3 at height 1.0. This is the per-layer "height
// amplitude" convention. With every contrast EQUAL to c, e_k = c * h_k, and the result is the same
// as the m19.7c form, term for term.
//
// WHY e_ref IS TAKEN OVER PAINTED LAYERS ONLY. A layer the painter did not put here (w = 0) must
// have no say in this pixel, not even through the reference. So e_ref is taken over
// PAINTED layers only, and an unpainted layer's exponent is clamped at 0 below.
//
// WHY exp2 AND NOT A MAX-HEIGHT GATE. The gate ("keep whatever is within some depth of the highest
// layer, drop the rest") was proposed and rejected in ADR-0066: it can cut a layer to exactly
// zero, so a layer painted at 1% that happens to be highest can end up owning the pixel. Here
// g_k > 0 always and b_k carries the factor w_k: a layer's share is 0 where its painted weight is
// 0, and grows continuously and monotonically with that weight. Contrast only STEEPENS the
// transition the painter drew; it cannot move it to wherever a stray texel is tallest.
//
// THE BIAS AT UNEQUAL CONTRASTS. By design, two layers at the SAME height h > 0 with DIFFERENT
// contrasts have different e, so b != w: the higher-contrast layer is the taller one and takes
// more of the pixel. The bypass below covers only equal e, so this is not a bug, but it means the
// painter's weights are not what the picture shows wherever contrasts differ. Give layers that
// meet the same contrast if the painter must get exactly the weights they painted.
//
// WHY THE BYPASS. When every painted layer has the same e (always true of untextured layers, whose
// fallback height is 0; and of every layer when all contrasts are 0), every g is 1 and the formula
// reduces to w / sum(w) — mathematically w, but NOT bit for bit: the four float weights do not
// sum to exactly 1.0, so the division moves them by an ULP. Returning w itself keeps every
// earlier bit-identity anchor (ADR-0063 §4 onwards) exact, and makes "no height data" cost
// nothing. A pixel with no painted layer also returns w (all zero).
//
// Returns b; the caller's difference-form blend reads b.yzw (b.x is implied, like w0 before it).
vec4 height_blend(vec4 w, vec4 h, vec4 c) {
    const vec4 e = c * h; // effective height, in halvings; >= 0 since c >= 0 and h is in [0,1]
    float e_ref = -1.0;   // so -1 marks "no painted layer yet"; any real e is >= 0
    float e_min = 1e30;
    for (int k = 0; k < 4; ++k) {
        if (w[k] > 0.0) {
            e_ref = max(e_ref, e[k]);
            e_min = min(e_min, e[k]);
        }
    }
    if (e_ref < 0.0 || e_ref == e_min) {
        return w; // the bypass (also the no-painted-layer case)
    }
    // min(.., 0) changes nothing for a painted layer (e_k <= e_ref by construction). For an
    // UNPAINTED one that is taller than e_ref it keeps g <= 1: a large contrast would otherwise
    // overflow exp2 to +inf, and w * g = 0 * inf is NaN, which would poison the whole sum.
    const vec4 q = w * exp2(min(e - vec4(e_ref), vec4(0.0)));
    // Never zero: the painted layer AT e_ref has g = 1 and w > 0.
    return q / (q.x + q.y + q.z + q.w);
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
    // m19.8d2: the LOD vectors. The vertex stage reads them (the derivative normal below is taken
    // from the MORPHED position, so it is the drawn triangle's plane at every morph — see
    // terrain.vert). m19.8d3: the fragment stage reads lod_tile.z's bake flags and grid.w.
    vec4 lod_origin;
    vec4 lod_camera;
    ivec4 lod_tile;
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
    //
    // m19.7c: the weights that enter this form are height_blend()'s b, not the sampled w. The
    // argument is unchanged — it never needed sum = 1 to the bit, only zero differences.
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

        // ── THE LAYER TEXTURES (m19.7b), AT WORLD-SPACE UVs ───────────────────────────────────
        //
        // The coordinate is WORLD xz over the layer's period, not tile-local xz. A tile-local
        // coordinate restarts at every tile's origin, so unless every tile's size happened to be a
        // whole number of periods, the pattern would JUMP at each seam — a visible grid laid over
        // the landscape exactly where the streaming system cut it. World xz is one continuous
        // function across all tiles, so the seam is invisible by construction (shown in the
        // m19.7b continuity proof). The cost: a world coordinate loses precision far from the
        // origin (at 10 km an f32 resolves ~1 mm, still far below a texel), which world-origin
        // rebasing will answer when worlds get that large.
        //
        // The colour each layer contributes is its base colour TINTED by its texture. Every
        // layer's colour is formed BEFORE the blend, and the blend below is m19.4's difference
        // form, unchanged — so with the white fallback (exactly 1.0) every c_k is the scalar
        // colour to the bit, and every earlier anchor still holds exactly.
        const vec2 xz = v_world.xz;
        const vec4 t0 = texture(layer_tex0, xz / splat.uv_scale[0].xy);
        const vec4 t1 = texture(layer_tex1, xz / splat.uv_scale[0].zw);
        const vec4 t2 = texture(layer_tex2, xz / splat.uv_scale[1].xy);
        const vec4 t3 = texture(layer_tex3, xz / splat.uv_scale[1].zw);
        // m19.7c: the painted weights, redistributed by the four layer heights (texture A,
        // linear) — height_blend() above. Everything below blends with b where m19.7b used w.
        const vec4 h = vec4(t0.a, t1.a, t2.a, t3.a);
        const vec4 b = height_blend(w, h, splat.height_contrast);

        const vec3 c0 = splat.color[0].rgb * t0.rgb;
        const vec3 c1 = splat.color[1].rgb * t1.rgb;
        const vec3 c2 = splat.color[2].rgb * t2.rgb;
        const vec3 c3 = splat.color[3].rgb * t3.rgb;
        base = c0 + b.g * (c1 - c0) + b.b * (c2 - c0) + b.a * (c3 - c0);
        const float m0 = splat.color[0].w;
        metallic = m0 + b.g * (splat.color[1].w - m0) + b.b * (splat.color[2].w - m0) +
                   b.a * (splat.color[3].w - m0);
        const float r0 = splat.roughness.x;
        roughness = r0 + b.g * (splat.roughness.y - r0) + b.b * (splat.roughness.z - r0) +
                    b.a * (splat.roughness.w - r0);
    }
    // ── m19.8d3: THE BAKED APPEARANCE OF A COARSE TILE, AND THE FADE TOWARD THE PARENT'S ──────
    //
    // A parent tile has no splat palette (its level-0 tiles may not even share one); it shades
    // from its bake, sampled at this pixel's position in its own sample grid.
    if ((pc.lod_tile.z & kOwnBake) != 0) {
        vec2 mr;
        bake_sample(bake_color, bake_material, v_lod.xy, base, mr);
        metallic = mr.x;
        roughness = mr.y;
    }
    // A MORPHING tile — a level-0 tile with its full detail above, or a parent with its own bake —
    // fades toward its PARENT's bake by the factor the vertex stage moved its geometry by. At
    // m = 1 the surface is the parent's and so is the shading, so swapping this tile for its
    // parent (or the reverse) changes nothing on screen: the geometry pop and the appearance pop
    // are removed by the same number.
    //
    // WHERE in the parent's bake: this tile is one quadrant of its parent, and its sample (i, j)
    // is the parent's sample ((offset + i) / 2, (offset + j) / 2), offset = 0 or N - 1 by
    // quadrant. Even vertices land on a parent texel, odd ones exactly halfway between two —
    // the lattice the geometry morph uses.
    //
    // (1 - m) * own + m * parent, not mix(): both endpoints are then exact (terrain.vert's rule).
    if ((pc.lod_tile.z & kParentBake) != 0) {
        const float m = clamp(v_lod.z, 0.0, 1.0);
        if (m > 0.0) {
            const vec2 quadrant = vec2((pc.lod_tile.z & kQuadrantX) != 0 ? 1.0 : 0.0,
                                       (pc.lod_tile.z & kQuadrantZ) != 0 ? 1.0 : 0.0);
            const vec2 sp = 0.5 * (quadrant * (pc.grid.w - 1.0) + v_lod.xy);
            vec3 parent_base;
            vec2 parent_mr;
            bake_sample(parent_bake_color, parent_bake_material, sp, parent_base, parent_mr);
            base = (1.0 - m) * base + m * parent_base;
            metallic = (1.0 - m) * metallic + m * parent_mr.x;
            roughness = (1.0 - m) * roughness + m * parent_mr.y;
        }
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
    // NO geometric specular AA here, deliberately (ADR-0078 step 1a applies it in pbr_forward*).
    // The filter measures how fast the SHADING normal turns across a pixel, and terrain's `n` is a
    // per-triangle FACE normal rebuilt from position derivatives just above. Within one facet that
    // normal is constant, so dFdx(n) is zero and the filter does nothing; across a facet edge the
    // 2x2 quad straddles two facets and the derivative is large for a reason that is not curvature,
    // so the filter would widen roughness along a one-pixel line at every triangle edge. That is an
    // artefact bought for no benefit. Terrain needs a SMOOTH normal first -- one differentiated from
    // the heightfield analytically, or sampled from a baked normal map -- and then this filter
    // becomes correct here. That is a separate brick; see ADR-0078.

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
        // kept instruction for instruction, so a tile without a sky renders as m19.5 did (to one
        // f16 ULP: a driver may contract this branch differently now the shader holds another;
        // ADR-0065 §4). A uniform white environment of irradiance E reflects diffuse
        // (1-metallic)*base*E and a specular term of about f0*E, with f0 = 0.04 for dielectrics
        // and the base colour for metals. A flat-ambient metal reads DARKER than a real one: a
        // metal has no diffuse, so its only ambient light is this f0 term, where a real metal
        // mirrors the sky — the branch above.
        radiance += pc.surface.w * ((1.0 - metallic) * base + f0);
    }
    out_color = vec4(radiance, 1.0);
}
