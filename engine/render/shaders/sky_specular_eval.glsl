// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Reading the prefiltered sky specular (ADR-0078 section 2): the lookup half of the split sum whose
// bakes are sky_specular_prefilter.comp and sky_specular_dfg.comp.
//
// The includer must define SKY_PREFILTERED_BINDING and SKY_DFG_BINDING (the slots differ per
// pipeline and a header has no business choosing a descriptor slot in someone else's set), and
// must have declared `skyview_lut` and included sky_mapping.glsl first: mip 0 of the chain IS the
// sky-view LUT, so the lookup needs it.
#ifndef RIME_SKY_SPECULAR_EVAL_GLSL
#define RIME_SKY_SPECULAR_EVAL_GLSL

#ifndef SKY_PREFILTERED_BINDING
#error "define SKY_PREFILTERED_BINDING before including sky_specular_eval.glsl"
#endif
#ifndef SKY_DFG_BINDING
#error "define SKY_DFG_BINDING before including sky_specular_eval.glsl"
#endif

layout(set = 0, binding = SKY_PREFILTERED_BINDING) uniform sampler2DArray sky_prefiltered;
layout(set = 0, binding = SKY_DFG_BINDING) uniform sampler2D sky_dfg;

// The direction to look the prefiltered sky up in (Lagarde & de Rousiers, "Moving Frostbite to
// Physically Based Rendering", SIGGRAPH 2014, section 4.9.2). The bake assumed the lobe is centred
// on the mirror direction r, but a rough surface's reflection lobe really leans toward the surface
// normal (the microfacet masking term cuts the grazing side off), so as roughness rises the useful
// lookup direction slides from r toward n. Without it a rough floor's highlight sits too far from
// the viewer and a rough sphere's limb looks like a mirror ball.
vec3 sky_specular_dominant_direction(vec3 n, vec3 r, float roughness) {
    const float alpha = roughness * roughness;
    const float s = (1.0 - alpha) * (sqrt(1.0 - alpha) + alpha);
    return normalize(mix(n, r, s));
}

// The sky, filtered to `roughness`, as seen along `r`. The chain has (layers + 1) levels: level 0
// is the sky-view LUT itself, level k >= 1 is array layer k - 1, and level k is the lobe of
// perceptual roughness k / layers. Roughness therefore maps LINEARLY to a fractional level, and the
// two neighbouring levels are blended by the fraction -- the lerp across mips a trilinear sampler
// would do, written by hand because the chain is an array, not a mip pyramid.
//
// The level count is read from the texture rather than duplicated as a constant, so the bake and
// this lookup cannot disagree about it.
vec3 sky_specular_prefiltered(vec3 r, float roughness) {
    const float layers = float(textureSize(sky_prefiltered, 0).z);
    const float level = clamp(roughness, 0.0, 1.0) * layers;
    const float lo = floor(level);
    const float frac = level - lo;
    const vec2 uv = skyview_uv_from_direction(r);
    const vec3 a = lo < 0.5 ? texture(skyview_lut, uv).rgb
                            : texture(sky_prefiltered, vec3(uv, lo - 1.0)).rgb;
    if (frac <= 0.0) {
        return a;
    }
    const vec3 b = texture(sky_prefiltered, vec3(uv, min(lo, layers - 1.0))).rgb;
    return mix(a, b, frac);
}

// The BRDF half of the split sum: what fraction of a uniform white sky a surface of Fresnel `f0`
// reflects, at this roughness and view angle. f0 * A + B straight from the table, then multiplied
// by an ENERGY-COMPENSATION factor.
//
// WHY COMPENSATE. The table integrates ONE bounce: light that hits a microfacet and leaves. On a
// rough surface light also scatters between facets before escaping, and a single-bounce BRDF throws
// that energy away, so a rough white metal comes out DARKER than the physics says (the amount is
// measured in sky_specular_test.cpp). Following Filament (and Fdez-Aguera 2019), we put the
// missing energy back with the simplest model that conserves it: assume the multiply-scattered
// light has the same shape as the single-scattered, so it is the same lobe scaled by
// 1 + f0 * (1/E - 1), where E = A + B is the table's own directional albedo for a perfect
// (f0 = 1) reflector. At f0 = 1 the factor is exactly 1/E, so E * (1/E) = 1: a white metal under a
// white sky returns the sky, the white-furnace test. At f0 = 0 it is 1 (a dielectric's sheen gains
// nothing). In between it is a plausible interpolation that must never exceed 1 -- which the test
// checks over the whole (f0, n.v, roughness) domain rather than assuming.
vec3 sky_specular_environment_brdf(vec3 f0, float roughness, float n_dot_v) {
    const vec2 ab =
        texture(sky_dfg, vec2(clamp(n_dot_v, 0.0, 1.0), clamp(roughness, 0.0, 1.0))).rg;
    const float e = max(ab.x + ab.y, 1.0e-4);
    return (f0 * ab.x + ab.y) * (vec3(1.0) + f0 * (1.0 / e - 1.0));
}

#endif // RIME_SKY_SPECULAR_EVAL_GLSL
