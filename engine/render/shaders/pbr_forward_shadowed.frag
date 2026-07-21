// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Forward-PBR fragment shader WITH directional cascaded shadow maps (m10.1, ADR-0032 §3). This is
// pbr_forward.frag verbatim — same Cook-Torrance BRDF, same everything (see that file / docs/math/
// pbr.md for the shading derivation) — plus one addition: the PRIMARY directional light's
// contribution is modulated by a shadow factor sampled from the cascaded shadow map. It is a
// SEPARATE shader from pbr_forward.frag on purpose: with shadows off the renderer binds the
// unmodified pbr_forward pipeline and is byte-identical to the M5.6 baseline (ADR-0032 §11). The
// shared BRDF is duplicated here rather than #included because the offline shader compile has no
// include path configured yet; factoring a common lighting_data.glsl is the C5 follow-up.
//
// The shadow test itself (why "render depth from the light, compare from the camera" answers "is
// this point occluded", the cascade selection, PCF, and bias) is derived in
// docs/math/shadow-mapping.md.
#version 450

struct DirLight {
    vec4 direction; // xyz = the direction the light TRAVELS (world), w unused
    vec4 radiance;  // rgb = color * intensity (linear), w unused
};

struct PointLight {
    vec4 position; // xyz = world position, w = falloff radius
    vec4 radiance; // rgb = color * intensity (linear), w unused
};

layout(std140, set = 0, binding = 0) uniform FrameUniforms {
    mat4 view_proj;
    vec4 camera_pos;
    vec4 ambient;
    uvec4 light_counts;
    DirLight dir_lights[4];
    PointLight point_lights[16];
} frame;

layout(std140, set = 0, binding = 1) uniform DrawUniforms {
    mat4 model;
    mat4 normal_matrix;
    vec4 base_color;
    vec4 params;   // x = metallic, y = roughness, z = normal_scale, w = occlusion_strength
    vec4 emissive; // rgb = emissive factor (linear)
} draw;

layout(set = 0, binding = 2) uniform sampler2D base_color_tex;
layout(set = 0, binding = 3) uniform sampler2D metallic_roughness_tex; // G = roughness, B = metallic
layout(set = 0, binding = 4) uniform sampler2D normal_tex;             // tangent-space normal
layout(set = 0, binding = 5) uniform sampler2D occlusion_tex;          // R = ambient occlusion
layout(set = 0, binding = 6) uniform sampler2D emissive_tex;

// The cascaded shadow map (m10.1): a depth array (one layer per cascade) sampled with hardware
// depth-compare — texture() returns the fraction of the 2×2 neighbourhood that passes the compare,
// i.e. bilinear PCF for free; the 3×3 loop below widens it.
layout(set = 0, binding = 7) uniform sampler2DArrayShadow shadow_map;

layout(std140, set = 0, binding = 8) uniform ShadowUniforms {
    mat4 cascade_view_proj[4]; // light clip-from-world per cascade
    vec4 params;               // x = cascade count, y = PCF radius (texels), z = depth bias, w = normal bias
    vec4 texel;                // x = 1 / shadow-map resolution
} shadow;

// The local-light (spot) shadow maps (m10.2): one depth-array layer per shadowing spot, sampled with
// the same hardware depth-compare as the cascades. Bound from LocalShadowMap::add — a persistent,
// destruction-invalidated array, but from the shader's side it is just another sampler2DArrayShadow.
layout(set = 0, binding = 9) uniform sampler2DArrayShadow local_shadow_map;

struct SpotShadow {
    mat4 view_proj;         // light clip-from-world (perspective)
    vec4 pos_range;         // xyz world position, w = range
    vec4 dir_cos_inner;     // xyz unit travel direction, w = cos(inner cone half-angle)
    vec4 radiance_cos_outer; // rgb color*intensity, w = cos(outer cone half-angle)
};

layout(std140, set = 0, binding = 10) uniform LocalShadowUniforms {
    SpotShadow spots[8];    // kMaxLocalShadows
    vec4 params;            // x = spot count, y = PCF radius (texels), z = depth bias, w = normal bias
    vec4 texel;             // x = 1 / local-shadow resolution
} local;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_world_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_world_tangent;
layout(location = 4) in float v_tangent_w;

layout(location = 0) out vec4 out_hdr;

const float kPi = 3.14159265358979;

float d_ggx(float n_dot_h, float alpha) {
    float a2 = alpha * alpha;
    float t = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * t * t);
}

float v_smith_ggx(float n_dot_v, float n_dot_l, float alpha) {
    float a2 = alpha * alpha;
    float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 f_schlick(float v_dot_h, vec3 f0) {
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

vec3 shade_light(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, float metallic, float alpha) {
    float n_dot_l = dot(n, l);
    if (n_dot_l <= 0.0)
        return vec3(0.0);
    vec3 h = normalize(v + l);
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(v, h), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = f_schlick(v_dot_h, f0);
    vec3 specular = d_ggx(n_dot_h, alpha) * v_smith_ggx(n_dot_v, n_dot_l, alpha) * fresnel;

    vec3 kd = (vec3(1.0) - fresnel) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / kPi;

    return (diffuse + specular) * radiance * n_dot_l;
}

vec3 perturb_normal(vec3 world_normal) {
    vec3 N = normalize(world_normal);
    vec3 T = normalize(v_world_tangent - N * dot(N, v_world_tangent));
    vec3 B = v_tangent_w * cross(N, T);
    vec3 c = texture(normal_tex, v_uv).xyz;
    vec3 n_tangent = vec3((2.0 * c.xy - 1.0) * draw.params.z, 2.0 * c.z - 1.0);
    return normalize(mat3(T, B, N) * n_tangent);
}

// The sun's shadow factor at this fragment: 1 = fully lit, 0 = fully shadowed. Walk the cascades
// coarse-to-fine and use the FIRST one the fragment projects inside — that is the tightest map that
// still covers it (docs/math/shadow-mapping.md §2). Offset the sample point along the surface normal
// first (normal bias) to fight self-shadowing acne at grazing sun angles, then compare with a small
// constant depth bias. A 3×3 grid of hardware-compare taps softens the edge (PCF, §5).
float sun_shadow(vec3 world_pos, vec3 N) {
    uint count = uint(shadow.params.x);
    if (count == 0u)
        return 1.0; // shadows disabled — no modulation (kept for safety; the off path uses pbr_forward)

    vec3 biased = world_pos + N * shadow.params.w;
    for (uint c = 0u; c < count && c < 4u; ++c) {
        vec4 lc = shadow.cascade_view_proj[c] * vec4(biased, 1.0);
        vec3 p = lc.xyz / lc.w;         // ortho ⇒ w = 1, but the divide is harmless and general
        vec2 uv = p.xy * 0.5 + 0.5;     // clip [-1,1] → texture [0,1] (both y-down, Vulkan)
        // Inside this cascade's map? (a hair inside the border avoids sampling the clamped edge)
        if (all(greaterThanEqual(uv, vec2(0.001))) && all(lessThanEqual(uv, vec2(0.999))) &&
            p.z >= 0.0 && p.z <= 1.0) {
            float ref = p.z - shadow.params.z;   // the receiver's light-space depth, biased toward lit
            float step = shadow.params.y * shadow.texel.x;
            float sum = 0.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    vec2 o = vec2(float(dx), float(dy)) * step;
                    // sampler2DArrayShadow: (u, v, layer, reference) → filtered compare result.
                    sum += texture(shadow_map, vec4(uv + o, float(c), ref));
                }
            }
            return sum / 9.0;
        }
    }
    return 1.0; // past the last cascade — treat as lit rather than pop to hard shadow
}

// A spot light's shadow factor at this fragment: 1 = lit, 0 = shadowed. The spot is a perspective
// shadow map (its own array layer `idx`), so unlike the ortho cascades the projection is a real
// perspective divide. Same normal + depth bias and 3×3 hardware PCF as the sun (docs/math/shadow-
// mapping.md §6). A fragment outside the spot's map is lit (the cone falloff, not the map, bounds it).
float spot_shadow(uint idx, vec3 world_pos, vec3 N) {
    vec3 biased = world_pos + N * local.params.w;
    vec4 lc = local.spots[idx].view_proj * vec4(biased, 1.0);
    vec3 p = lc.xyz / lc.w;      // perspective divide (w ≠ 1 here)
    vec2 uv = p.xy * 0.5 + 0.5;  // clip [-1,1] → texture [0,1] (both y-down, Vulkan)
    if (any(lessThan(uv, vec2(0.001))) || any(greaterThan(uv, vec2(0.999))) || p.z < 0.0 || p.z > 1.0)
        return 1.0;
    float ref = p.z - local.params.z;
    float step = local.params.y * local.texel.x;
    float sum = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 o = vec2(float(dx), float(dy)) * step;
            sum += texture(local_shadow_map, vec4(uv + o, float(idx), ref));
        }
    }
    return sum / 9.0;
}

void main() {
    vec3 n = perturb_normal(v_world_normal);
    vec3 v = normalize(frame.camera_pos.xyz - v_world_pos);
    vec3 albedo = draw.base_color.rgb * texture(base_color_tex, v_uv).rgb;

    vec2 mr = texture(metallic_roughness_tex, v_uv).gb;
    float metallic = draw.params.x * mr.y;
    float roughness = clamp(draw.params.y * mr.x, 0.045, 1.0);
    float alpha = roughness * roughness;

    float ao = mix(1.0, texture(occlusion_tex, v_uv).r, draw.params.w);
    vec3 out_radiance = albedo * frame.ambient.rgb * ao;

    // The sun (light 0) is the shadow caster; its contribution is scaled by the cascade shadow
    // factor. The remaining directional lights (fill lights) are unshadowed in v1.
    float sun_factor = sun_shadow(v_world_pos, normalize(v_world_normal));
    for (uint i = 0u; i < frame.light_counts.x && i < 4u; ++i) {
        vec3 l = normalize(-frame.dir_lights[i].direction.xyz);
        vec3 contrib =
            shade_light(n, v, l, frame.dir_lights[i].radiance.rgb, albedo, metallic, alpha);
        out_radiance += (i == 0u) ? contrib * sun_factor : contrib;
    }

    for (uint i = 0u; i < frame.light_counts.y && i < 16u; ++i) {
        vec3 to_light = frame.point_lights[i].position.xyz - v_world_pos;
        float dist2 = max(dot(to_light, to_light), 1e-4);
        float dist = sqrt(dist2);
        vec3 l = to_light / dist;
        float r = max(frame.point_lights[i].position.w, 1e-3);
        float q = dist / r;
        float q4 = q * q * q * q;
        float window = clamp(1.0 - q4, 0.0, 1.0);
        float falloff = window * window / dist2;
        vec3 radiance = frame.point_lights[i].radiance.rgb * falloff;
        out_radiance += shade_light(n, v, l, radiance, albedo, metallic, alpha);
    }

    // Spot lights (m10.2): a point light with a cone, each casting a real shadow through its own
    // perspective shadow map. Distance falloff is the point-light window; the cone falloff runs from
    // full inside the inner half-angle to zero at the outer one (smoothstep on the cosines).
    uint spot_count = uint(local.params.x);
    for (uint i = 0u; i < spot_count && i < 8u; ++i) {
        vec3 to_light = local.spots[i].pos_range.xyz - v_world_pos;
        float dist2 = max(dot(to_light, to_light), 1e-4);
        float dist = sqrt(dist2);
        vec3 l = to_light / dist;
        float r = max(local.spots[i].pos_range.w, 1e-3);
        float q = dist / r;
        float q4 = q * q * q * q;
        float window = clamp(1.0 - q4, 0.0, 1.0);
        float dist_falloff = window * window / dist2;
        // Cone: cos of the angle between the light's axis and the light→fragment ray.
        float cos_theta = dot(local.spots[i].dir_cos_inner.xyz, -l);
        float cone = smoothstep(local.spots[i].radiance_cos_outer.w, local.spots[i].dir_cos_inner.w,
                                cos_theta);
        if (cone <= 0.0)
            continue;
        vec3 radiance = local.spots[i].radiance_cos_outer.rgb * dist_falloff * cone;
        vec3 contrib = shade_light(n, v, l, radiance, albedo, metallic, alpha);
        out_radiance += contrib * spot_shadow(i, v_world_pos, normalize(v_world_normal));
    }

    out_radiance += draw.emissive.rgb * texture(emissive_tex, v_uv).rgb;
    out_hdr = vec4(out_radiance, 1.0);
}
