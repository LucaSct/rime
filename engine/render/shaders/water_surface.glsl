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


// Water slope field: a sum of travelling sine waves, one per octave of wavelength.
//
// A heightfield h(p,t) = sum a_i sin(k_i.p - w_i t) has slope grad h = sum a_i k_i cos(...) dir_i,
// so we sum SLOPES directly and never need the height. Each term carries a target slope (the
// steepness a_i k_i, dimensionless), which is what the eye reads as glitter: rms ~0.09 is a
// breezy pond. Frequencies follow the capillary-gravity dispersion w = sqrt(g k (1 + (k/370)^2)),
// so long waves crawl and short ripples dart, which is what makes the motion look like water
// rather than a texture scrolling. Directions fan +-40 degrees around the wind, and phases are
// irrational offsets so no two terms line up into a visible lattice.
//
// Anti-aliasing: a wave shorter than ~4 pixels of footprint only produces shimmering noise, so each
// term fades out as wavelength/footprint falls. The residual roughness is supplied by the
// specular filter, which is the same energy the faded waves would have carried.
vec2 water_slope(vec2 p, float t, float footprint) {
    const float kWavelength[8] = float[](4.1, 2.3, 1.27, .73, .41, .235, .135, .078);
    const float kSlope[8] = float[](.020, .030, .042, .052, .056, .050, .040, .030);
    const float kAngle[8] = float[](.00, .55, -.62, .31, -.28, .70, -.74, .12);
    vec2 s = vec2(0.0);
    for (int i = 0; i < 8; ++i) {
        float k = 6.2831853 / kWavelength[i];
        float w = sqrt(9.81 * k * (1.0 + (k / 370.0) * (k / 370.0)));
        float ang = .3398 + kAngle[i]; // wind direction (0.94, 0.34) plus the fan offset
        vec2 d = vec2(cos(ang), sin(ang));
        float resolved = smoothstep(2.0, 8.0, kWavelength[i] / footprint);
        s += d * (kSlope[i] * resolved * cos(k * dot(d, p) - w * t + float(i) * 1.7));
        // A crossing train at a different angle and speed breaks the straight crests into chop.
        vec2 e = vec2(cos(ang + 1.9), sin(ang + 1.9));
        s += e * (kSlope[i] * .55 * resolved *
                  cos(k * 1.13 * dot(e, p) - w * 0.83 * t + float(i) * 2.9 + 1.0));
    }
    return s;
}

PhaseOptics phase_optics(vec3 geometric, vec3 detail_normal, vec3 tangent, vec3 position,
                        vec2 uv, vec3 view, vec3 tint, vec3 frost_map,
                        float authored_roughness, vec4 phase, vec3 presentation) {
    PhaseOptics o;
    float wet = phase.y > 2.5 ? 0.0 : phase.z;
    float frost = phase.y > 2.5 ? 1.0 : 0.0;
    vec3 N = normalize(geometric);
    vec3 T = normalize(tangent - N * dot(N, tangent));
    vec3 B = normalize(cross(N, T));
    // Living surface: a spectrum of travelling slope waves (see water_slope above). Static frost
    // slopes still fade with the exact liquid fraction, avoiding the old 1/8 atlas jumps. The
    // clock is committed simulation time, so paused/refused ticks remain still.
    vec2 p = position.xz;
    float footprint = max(length(fwidth(position)), 1e-4);
    vec2 slope = water_slope(p, presentation.x, footprint);
    vec3 mapped = detail_normal / max(dot(detail_normal, N), .25) - N;
    o.normal = normalize(N + mapped * mix(.18, .06, wet) +
                         wet * vec3(-slope.x, 0.0, -slope.y));
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
