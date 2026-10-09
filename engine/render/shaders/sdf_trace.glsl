// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Shared clipmap access and traces. Slots belong to the includer, as in sky_specular_eval.glsl.
#ifndef RIME_SDF_TRACE_GLSL
#define RIME_SDF_TRACE_GLSL

#ifndef SDF_LEVEL0_BINDING
#error "define SDF_LEVEL0_BINDING before including sdf_trace.glsl"
#endif
#ifndef SDF_LEVEL1_BINDING
#error "define SDF_LEVEL1_BINDING before including sdf_trace.glsl"
#endif
#ifndef SDF_LEVEL2_BINDING
#error "define SDF_LEVEL2_BINDING before including sdf_trace.glsl"
#endif
#ifndef SDF_LEVELS_BINDING
#error "define SDF_LEVELS_BINDING before including sdf_trace.glsl"
#endif

layout(set = 0, binding = SDF_LEVEL0_BINDING) uniform sampler3D level0;
layout(set = 0, binding = SDF_LEVEL1_BINDING) uniform sampler3D level1;
layout(set = 0, binding = SDF_LEVEL2_BINDING) uniform sampler3D level2;

struct LevelInfo {
    vec4 origin_extent; // xyz = world origin (voxel (0,0,0)'s corner), w = world extent
    vec4 band_voxel;    // x = band, y = voxel size, z/w unused
};
layout(std140, set = 0, binding = SDF_LEVELS_BINDING) uniform Levels {
    LevelInfo info[3];
} clipmap;

float sample_level(int i, vec3 uvw) {
    if (i == 0) return texture(level0, uvw).r;
    if (i == 1) return texture(level1, uvw).r;
    return texture(level2, uvw).r;
}

float sdf_sample(vec3 world_pos) {
    for (int i = 0; i < 3; ++i) {
        vec4 oe = clipmap.info[i].origin_extent;
        vec3 uvw = (world_pos - oe.xyz) / oe.w;
        if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)))) {
            return sample_level(i, uvw) * clipmap.info[i].band_voxel.x;
        }
    }
    vec4 oe2 = clipmap.info[2].origin_extent;
    vec3 uvw2 = clamp((world_pos - oe2.xyz) / oe2.w, 0.0, 1.0);
    return sample_level(2, uvw2) * clipmap.info[2].band_voxel.x;
}

float sdf_sphere_trace(vec3 origin, vec3 dir, float max_dist) {
    float t = 0.0;
    const float kHitEpsilon = 0.001;
    const int kMaxSteps = 128;
    for (int i = 0; i < kMaxSteps; ++i) {
        if (t >= max_dist) {
            break;
        }
        float d = sdf_sample(origin + dir * t);
        if (d < kHitEpsilon) {
            return t;
        }
        t += max(d, kHitEpsilon);
    }
    return -1.0;
}

// The field's gradient by central differences, normalized — the standard way to recover a surface
// NORMAL from an SDF (the field has no separate normal channel; its own shape IS the geometry).
// `eps` is half the FINEST clipmap level's voxel size: fine enough to resolve real surface detail
// at the level actually doing the tracing, coarse enough to stay well above the field's own
// quantization noise (docs/math/sdf.md's narrow-band error).
vec3 sdf_normal(vec3 p, float eps) {
    vec3 d;
    d.x = sdf_sample(p + vec3(eps, 0.0, 0.0)) - sdf_sample(p - vec3(eps, 0.0, 0.0));
    d.y = sdf_sample(p + vec3(0.0, eps, 0.0)) - sdf_sample(p - vec3(0.0, eps, 0.0));
    d.z = sdf_sample(p + vec3(0.0, 0.0, eps)) - sdf_sample(p - vec3(0.0, 0.0, eps));
    return normalize(d);
}

// Quilez's SDF soft-shadow estimator used as a specular occlusion integral: at distance t
// the cone radius is t*tan(theta); d/radius estimates its unoccluded fraction. The running
// min(), rather than a product, avoids counting one occluder again at successive march steps.
// This is a bounded approximation, not an integration of the full GGX tail.
float sdf_cone_occlusion(vec3 origin, vec3 dir, float tan_half_angle, float max_dist) {
    // Start TWO finest voxels away (0.25 m today): trilinear reconstruction has a voxel-sized
    // surface uncertainty. The caller also lifts the origin by this much along the normal,
    // so a grazing ray cannot stay inside its own reconstructed surface.
    float t = 2.0 * clipmap.info[0].band_voxel.y;
    float visibility = 1.0;
    // Sixteen samples, independent of DDGI's 128-step hit trace: a per-pixel sky reader must
    // fit a small lighting budget. Exhaustion preserves the minimum seen, so distant blockers
    // can be missed; the reach is deliberately only 8 m, not the whole 128 m clipmap.
    const int kConeSteps = 16;
    for (int step = 0; step < kConeSteps && t < max_dist; ++step) {
        vec3 p = origin + dir * t;
        int level = -1;
        for (int i = 0; i < 3; ++i) {
            vec4 oe = clipmap.info[i].origin_extent;
            vec3 uvw = (p - oe.xyz) / oe.w;
            if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)))) {
                level = i;
                break;
            }
        }
        // Outside the field there is no evidence of a blocker; unlike DDGI's hit trace we must
        // not clamp to an edge texel and turn that one edge into an infinite wall.
        if (level < 0) break;
        float d = sdf_sample(p);
        if (d <= 0.0) return 0.0;
        float band = clipmap.info[level].band_voxel.x;
        // R16Snorm is a NARROW-BAND field: +band means "at least band", not a measured distance.
        // Using band/radius as visibility would darken even an empty outdoor field. Only an
        // unsaturated sample is geometric evidence; saturated samples still take safe band steps.
        if (d < band * 0.999) {
            visibility = min(visibility, clamp(d / max(t * tan_half_angle, 1e-4), 0.0, 1.0));
        }
        t += max(d, clipmap.info[level].band_voxel.y * 0.5);
    }
    return visibility;
}

// GGX has slope CDF F(s)=s^2/(alpha^2+s^2), so its median half-vector slope is alpha.
// Reflection doubles the half-vector angle; in the small-angle limit tan(theta) ~= 2*alpha.
// We extend that finite slope to broad lobes as an approximation (the GGX tail is unbounded).
// lobe_roughness is the ACTUAL prefiltered lookup argument: alpha = lobe_roughness^2,
// hence forward's sqrt(AA-widened alpha) and resolve's stored roughness each trace their own lobe.
float sdf_sky_visibility(vec3 p, vec3 n, vec3 lookup, float lobe_roughness) {
    float alpha = lobe_roughness * lobe_roughness;
    vec3 origin = p + n * (2.0 * clipmap.info[0].band_voxel.y);
    return sdf_cone_occlusion(origin, lookup, max(2.0 * alpha, 1e-3), 8.0);
}

#endif // RIME_SDF_TRACE_GLSL
