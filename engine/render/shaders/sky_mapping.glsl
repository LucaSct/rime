// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky-view LUT's parameterisation (m17.7b): world direction <-> LUT texel, and nothing else.
//
// WHY THIS IS ITS OWN FILE, separate from sky_common.glsl. This mapping is purely geometric -- it
// declares no uniform block and reads no parameters -- so a shader can share it WITHOUT also
// inheriting a SkyParams binding it does not own. ssr_resolve.frag and ddgi_trace.comp sample the
// LUT but have no business declaring the sky's uniforms.
//
// THE PARAMETERISATION is Hillaire 2020's sky-view LUT mapping: azimuth is linear, and the
// elevation angle is stored with a SQUARE-ROOT warp that concentrates texels near the horizon,
// which is where a sky has all of its angular detail (the gradient changes fastest there, and it is
// where the eye actually looks). A linear-in-elevation map spends half its rows on the near-uniform
// dome overhead and blurs the horizon, which is exactly backwards.
//
// The forward and inverse below are EXACT inverses of each other; sky_lighting_test.cpp asserts the
// round trip rather than trusting the algebra here.
#ifndef RIME_SKY_MAPPING_GLSL
#define RIME_SKY_MAPPING_GLSL

const float kSkyPi = 3.14159265358979;

// Direction -> [0,1]^2 LUT coordinate.
vec2 skyview_uv_from_direction(vec3 dir) {
    const vec3 d = normalize(dir);
    // Elevation in [-pi/2, +pi/2]; t is its signed square-root warp in [-1, 1].
    const float l = asin(clamp(d.y, -1.0, 1.0));
    const float t = sign(l) * sqrt(abs(l) / (kSkyPi * 0.5));
    // Azimuth in (-pi, pi] -> [0, 1].
    const float a = atan(d.z, d.x);
    return vec2(a / (2.0 * kSkyPi) + 0.5, 0.5 + 0.5 * t);
}

// [0,1]^2 LUT coordinate -> unit direction. The exact inverse of the above: undoing the warp is
// t*t (not sqrt), which is what makes the two compose to the identity.
vec3 skyview_direction_from_uv(vec2 uv) {
    const float t = 2.0 * uv.y - 1.0;
    const float l = sign(t) * (t * t) * (kSkyPi * 0.5);
    const float a = (uv.x - 0.5) * 2.0 * kSkyPi;
    const float cl = cos(l);
    return vec3(cl * cos(a), sin(l), cl * sin(a));
}

#endif // RIME_SKY_MAPPING_GLSL
