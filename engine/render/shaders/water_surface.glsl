// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#ifndef RIME_WATER_SURFACE_GLSL
#define RIME_WATER_SURFACE_GLSL

struct PhaseOptics {
    vec3 normal;
    vec3 f0;
    vec3 transmission;
    float roughness;
};

PhaseOptics phase_optics(vec3 geometric, vec3 detail_normal, vec3 tangent, vec3 position,
                        vec2 uv, vec3 view, vec3 tint, vec3 frost_map,
                        float authored_roughness, vec4 phase, vec3 presentation) {
    PhaseOptics o;
    float wet = phase.y > 2.5 ? 0.0 : phase.z;
    float frost = phase.y > 2.5 ? 1.0 : 0.0;
    vec3 N = normalize(geometric);
    vec3 T = normalize(tangent - N * dot(N, tangent));
    vec3 B = normalize(cross(N, T));
    // Slope-space detail: continuous capillary waves ride the analytic Gerstner normal.
    // Static frost slopes fade with the exact liquid fraction, avoiding the old 1/8 atlas
    // jumps. The clock is committed simulation time, so paused/refused ticks remain still.
    vec2 p = position.xz;
    vec2 ripple = vec2(.94, .34) * (.026 * cos(dot(p, vec2(23.5, 8.5)) -
                                                           presentation.x * 8.0));
    ripple += vec2(.82, .57) * (.014 * cos(dot(p, vec2(39.4, 27.4)) -
                                                           presentation.x * 11.0 + 1.7));
    ripple += vec2(.99, -.09) * (.008 * cos(dot(p, vec2(73.3, -6.7)) -
                                                           presentation.x * 16.0 + 4.2));
    vec3 mapped = detail_normal / max(dot(detail_normal, N), .25) - N;
    o.normal = normalize(N + mapped * mix(.18, .06, wet) +
                         wet * (T * ripple.x + B * ripple.y));
    // Ice inclusions: the existing cellular frost field supplies branching internal cracks.
    // Sparse hashed air pockets add a second scale; their scattering fades continuously on
    // melting. Frost has more scattering and a wider lobe than polished bulk ice.
    float vein = presentation.z > .5 ? smoothstep(.65, .77, frost_map.r) : 0.0;
    vec2 cell = floor(uv * 48.0);
    float h = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);
    vec2 bubble_uv = fract(uv * 48.0) - vec2(.25 + h * .5, .5);
    float bubble = (1.0 - smoothstep(.025, .11, length(bubble_uv))) * step(.78, h);
    float cloudy = clamp(vein * .75 + bubble * .5 + frost, 0.0, 1.0) * (1.0 - wet);
    float ice_roughness = mix(.055, .29, clamp(vein + frost, 0.0, 1.0));
    // Keep the authored map responsive (including the environment smoke's roughness control),
    // while ordinary liquid texels land in the requested 0.02–0.06 glint range.
    float water_roughness = clamp(authored_roughness * .45, .025, .45);
    o.roughness = mix(mix(ice_roughness, .68, frost), water_roughness, wet);
    o.f0 = vec3(mix(.018, .020, wet));
    // Beer–Lambert absorption: optical depth is metres / refracted-ray cosine (Snell).
    // Red is absorbed fastest, leaving a cool blue-green column; a tapered bank makes
    // thin edges approach the authored bottom colour. This is a bottom-colour proxy,
    // not a scene-colour refraction pass: submerged geometry is not sampled here.
    vec2 edge_uv = uv * 2.0 - 1.0;
    float edge = presentation.y > .5 ? max(0.0, 1.0 - dot(edge_uv, edge_uv))
                                     : max(0.0, (1.0-edge_uv.x*edge_uv.x) *
                                                 (1.0-edge_uv.y*edge_uv.y));
    float thickness = phase.w * smoothstep(0.0, .55, edge);
    float nv = clamp(dot(o.normal, view), 0.0, 1.0);
    float cos_refracted = sqrt(1.0 - (1.0 - nv * nv) / (1.333 * 1.333));
    vec3 sigma = mix(vec3(2.4, .65, .28), vec3(4.2, 1.1, .55), wet);
    vec3 attenuation = exp(-sigma * thickness / cos_refracted);
    vec3 body = mix(vec3(.12, .30, .38), vec3(.012, .085, .12), wet);
    vec3 transmitted = tint * attenuation + body * (1.0 - attenuation);
    transmitted = mix(transmitted, vec3(.72, .84, .87), cloudy * .65);
    // Schlick energy split: the transmitted/scattered share is exactly the complement
    // of the reflected share. The specular lobe uses F0 through the existing sky/SSR path.
    o.transmission = transmitted * (vec3(1.0) - f_schlick(nv, o.f0));
    return o;
}
#endif
