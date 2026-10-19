// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Shared by mesh and terrain receivers. The includer supplies `shadow` and `shadow_map`.
// A stable texel grid does not by itself make cascade selection continuous: the two resolutions
// disagree at a silhouette. Blend to the next covering cascade near the current map's boundary,
// including its depth boundary. Interior pixels still perform only the original nine PCF taps.
#ifndef RIME_SUN_SHADOW_GLSL
#define RIME_SUN_SHADOW_GLSL

bool sun_cascade_coordinates(uint cascade, vec3 biased, out vec3 p, out vec2 uv) {
    vec4 clip = shadow.cascade_view_proj[cascade] * vec4(biased, 1.0);
    p = clip.xyz / clip.w;
    uv = p.xy * 0.5 + 0.5;
    // The hardware compare also filters half a texel beyond each tap. Derive the guard from
    // that footprint; a fixed UV epsilon changes meaning when map resolution or PCF changes.
    float guard = min(0.25, (max(shadow.params.y, 0.0) + 0.5) * shadow.texel.x);
    return all(greaterThanEqual(uv, vec2(guard))) &&
           all(lessThanEqual(uv, vec2(1.0 - guard))) && p.z >= 0.0 && p.z <= 1.0;
}

float sun_cascade_pcf(uint cascade, vec3 p, vec2 uv) {
    float ref = p.z - shadow.params.z;
    float step = shadow.params.y * shadow.texel.x;
    float sum = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 offset = vec2(float(dx), float(dy)) * step;
            sum += texture(shadow_map, vec4(uv + offset, float(cascade), ref));
        }
    }
    return sum / 9.0;
}

float sun_shadow(vec3 world_pos, vec3 N) {
    uint count = min(uint(shadow.params.x), 4u);
    vec3 biased = world_pos + N * shadow.params.w;
    for (uint c = 0u; c < count; ++c) {
        vec3 p;
        vec2 uv;
        if (!sun_cascade_coordinates(c, biased, p, uv))
            continue;
        float visibility = sun_cascade_pcf(c, p, uv);
        float guard = min(0.25, (max(shadow.params.y, 0.0) + 0.5) * shadow.texel.x);
        float xy_edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
        float z_edge = min(p.z, 1.0 - p.z);
        const float blend_width = 0.04;
        float weight = min(smoothstep(guard, guard + blend_width, xy_edge),
                           smoothstep(0.0, blend_width, z_edge));
        if (weight >= 1.0)
            return visibility;
        // Cascades are fitted to separate slices, so skip a coarser map that does not cover
        // this receiver. Beyond the final coverage, fade to the existing unshadowed fallback.
        for (uint next = c + 1u; next < count; ++next) {
            vec3 next_p;
            vec2 next_uv;
            if (sun_cascade_coordinates(next, biased, next_p, next_uv))
                return mix(sun_cascade_pcf(next, next_p, next_uv), visibility, weight);
        }
        return mix(1.0, visibility, weight);
    }
    return 1.0;
}

#endif
