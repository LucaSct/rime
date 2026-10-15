// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "sky_mapping.glsl"
layout(std140, set = 0, binding = 0) uniform LensUniforms {
    mat4 view_proj;
    mat4 model;
    mat4 normal_matrix;
    mat4 optic_from_world;
    mat4 world_from_optic;
    mat4 optical_view_proj;
    vec4 main_eye;
    vec4 aperture;
    vec4 optical;
    vec4 glass;
} lens;
layout(set = 0, binding = 1) uniform sampler2D scope_hdr;
#ifdef SKY_REFLECTION
layout(set = 0, binding = 2) uniform sampler2DArray sky_prefiltered;
#endif
layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_world_normal;
layout(location = 0) out vec4 out_color;

vec3 reflected_environment(vec3 direction) {
#ifdef SKY_REFLECTION
    // Existing chain: array layer k-1 has perceptual roughness k/layers. There is no
    // unfiltered sky-view input here, so clamp at the first GGX level instead of claiming mip 0.
    float layers = float(textureSize(sky_prefiltered, 0).z);
    float level = clamp(lens.glass.x * layers - 1.0, 0.0, layers - 1.0);
    vec2 uv = skyview_uv_from_direction(direction);
    vec3 a = texture(sky_prefiltered, vec3(uv, floor(level))).rgb;
    vec3 b = texture(sky_prefiltered, vec3(uv, min(floor(level) + 1.0, layers - 1.0))).rgb;
    return mix(a, b, fract(level));
#else
    return vec3(0.035); // neutral, low-energy glass reflection without an environment
#endif
}
void main() {
    vec3 p = (lens.optic_from_world * vec4(v_world_pos, 1.0)).xyz;
    vec3 eye = (lens.optic_from_world * vec4(lens.main_eye.xyz, 1.0)).xyz;
    if (lens.glass.z > 0.5) {
        // A holographic sight has unity magnification: transmit the world pixel behind this
        // physical glass, before the weapon overwrote it. Sampling the parent world snapshot
        // preserves exact perspective and avoids spending a second world render on a 1x sight.
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(scope_hdr, 0));
        vec3 transmitted = texture(scope_hdr, uv).rgb;
        // Parallel rays along the optic axis put the collimated reticle at p.xy = eye.xy.
        // Unlike painted texture marks, it stays on the sight line as the eye moves across glass.
        vec2 q = (p.xy - eye.xy) / lens.aperture.x;
        float aa = max(fwidth(q.x), fwidth(q.y));
        float ring = 1.0 - smoothstep(0.003, 0.003 + aa, abs(length(q) - 0.10));
        float dot_mark = 1.0 - smoothstep(0.006, 0.006 + aa, length(q));
        vec3 n = normalize(v_world_normal);
        vec3 v = normalize(lens.main_eye.xyz - v_world_pos);
        float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
        vec3 reflection = reflected_environment(reflect(-v, n));
        vec3 glass = transmitted * vec3(0.965, 0.985, 0.975) * lens.optical.z;
        out_color = vec4(glass * (1.0 - fresnel) + reflection * fresnel * lens.optical.w +
                         vec3(2.5, 0.018, 0.008) * max(ring, dot_mark) * lens.glass.y, 1.0);
        return;
    }
    vec2 aperture_uv = p.xy / lens.aperture.x;
    // Physical mesh/depth is the primary boundary; this also rejects a wrongly oversized disc.
    if (dot(aperture_uv, aperture_uv) > 1.0001) discard;

    // Thin-lens approximation, not an optical ray tracer: project a virtual focal plane through
    // the actual optic camera. Eye displacement changes the image slightly, and moves the exit
    // pupil strongly. Thus an off-axis/too-distant eye sees black glass, not a pasted camera feed.
    vec3 target = vec3(aperture_uv * lens.optical.x * lens.optical.y - eye.xy,
                       -lens.optical.x);
    vec4 optical_clip = lens.optical_view_proj * lens.world_from_optic * vec4(target, 1.0);
    vec2 uv = optical_clip.xy / max(optical_clip.w, 1.0e-6) * 0.5 + 0.5;
    float in_image = optical_clip.w > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) &&
                     all(lessThanEqual(uv, vec2(1.0))) ? 1.0 : 0.0;
    vec2 pupil = aperture_uv + eye.xy / lens.aperture.w;
    float radial = 1.0 - smoothstep(0.78, 1.0, length(pupil));
    float axial = 1.0 - smoothstep(0.5, 1.0,
                                  abs(eye.z - lens.aperture.y) / lens.aperture.z);
    float visible = radial * axial * in_image * step(0.0001, eye.z);
    vec3 transmitted = texture(scope_hdr, clamp(uv, vec2(0.0), vec2(1.0))).rgb;

    // Reticle is fixed to the optic axis (not the main view or a displaced pupil).
    vec2 q = aperture_uv;
    float aa = max(fwidth(q.x), fwidth(q.y));
    float crosshair = 1.0 - smoothstep(0.002, 0.002 + aa, min(abs(q.x), abs(q.y)));
    float marks = 0.0;
    for (int i = 1; i <= 4; ++i) {
        float d = abs(abs(q.y) - float(i) * 0.16);
        marks = max(marks, (1.0 - smoothstep(0.002, 0.002 + aa, d)) *
                          (1.0 - smoothstep(0.025, 0.025 + aa, abs(q.x))));
    }
    transmitted *= 1.0 - clamp(max(crosshair, marks) * lens.glass.y, 0.0, 1.0);
    vec3 n = normalize(v_world_normal);
    vec3 v = normalize(lens.main_eye.xyz - v_world_pos);
    float ndv = clamp(dot(n, v), 0.0, 1.0);
    float fresnel = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
    vec3 reflection = reflected_environment(reflect(-v, n));
    vec3 glass = transmitted * vec3(0.965, 0.985, 0.975) * lens.optical.z * visible;
    out_color = vec4(glass * (1.0 - fresnel) +
                     reflection * fresnel * lens.optical.w, 1.0);
}
