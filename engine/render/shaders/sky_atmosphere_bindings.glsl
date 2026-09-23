// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The physical inputs used by sky_lighting_radiance() when m17.7d swaps its analytic body. This is
// included BEFORE sky_common.glsl in both compute callers: GLSL needs globals declared before a
// function body can name them. Keeping the declarations in one binding-free include also prevents
// sky_skyview.comp and sky_sh.comp from drifting on the exact descriptor contract they share.
#ifndef RIME_SKY_ATMOSPHERE_BINDINGS_GLSL
#define RIME_SKY_ATMOSPHERE_BINDINGS_GLSL

layout(std140, set = 0, binding = 3) uniform AtmosphereParams {
    vec4 radii;
    vec4 rayleigh_scattering;
    vec4 mie_scattering;
    vec4 mie_absorption;
    vec4 ground_albedo;
    vec4 solar_irradiance;
}
atmosphere;

layout(set = 0, binding = 4) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 5) uniform sampler2D multiple_scattering_lut;

#endif // RIME_SKY_ATMOSPHERE_BINDINGS_GLSL
