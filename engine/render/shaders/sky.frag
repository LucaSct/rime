// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky (m17.0) — a procedural daytime sky with a sun disc and a layer of cloud, composited over
// the scene wherever nothing was drawn.
//
// WHAT THIS IS, AND WHAT IT IS NOT. This is an ANALYTIC sky, not a physical one. The colours come
// from a gradient fit and a Henyey-Greenstein-ish forward-scatter lobe around the sun; there is no
// Rayleigh/Mie integral, no transmittance function, no aerial perspective, and the sky does not
// light the scene. ADR-0040 records the intended end state -- a Hillaire-2020 precomputed-LUT
// atmosphere that IS the scene's light source -- and this pass is deliberately shaped so that model
// can replace the body of sky_radiance() without any other file moving: the pass seam (read scene
// colour + depth, write a second HDR target), the parameter block, and the sun coupling are the
// ones that design calls for.
//
// The technique for the clouds is the standard one: fractional Brownian motion (fBm) over a
// value-noise basis, evaluated where the view ray pierces a flat cloud slab, thresholded by a
// coverage parameter. Real volumetric clouds ray-march a 3-D density field and are a different
// (much more expensive) animal; this is the 2-D projection that reads correctly for a sky you look
// at rather than fly through.
#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_color; // the lit HDR frame
layout(set = 0, binding = 1) uniform sampler2D scene_depth; // D32, Vulkan NDC z in [0,1]

layout(std140, set = 0, binding = 2) uniform SkyParams {
    mat4 inv_view_proj;  // clip -> world, for the per-pixel view ray
    vec4 camera_pos;     // xyz world camera, w unused
    vec4 sun_dir;        // xyz normalized direction TOWARD the sun, w unused
    vec4 sun_radiance;   // rgb sun colour, a = angular radius of the disc (radians)
    vec4 zenith;         // rgb zenith colour, a = overall sky intensity
    vec4 horizon;        // rgb horizon colour, a = ground/below-horizon darkening
    vec4 cloud;          // x coverage [0,1], y density, z altitude (m), w scale (1/m)
    vec4 wind;           // xy scrolling offset (m), z cloud sharpness, w = enabled flag
}
sky;

layout(location = 0) out vec4 out_color;

// --- value noise + fBm ---------------------------------------------------------------------------
// A hash-based value noise: cheap, dependency-free, and stable across drivers because it is pure
// arithmetic on exactly-representable constants rather than a texture lookup.
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    // Quintic smoothstep (Perlin's improved fade): C2-continuous, so the fBm has no visible
    // derivative seams at cell boundaries the way the cubic one does.
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    // Five octaves, each double the frequency and half the amplitude. The 1.93 (rather than 2.0)
    // lacunarity and the small rotation each octave break up the axis-aligned grid the basis would
    // otherwise show at low coverage.
    const mat2 rot = mat2(0.80, 0.60, -0.60, 0.80);
    for (int i = 0; i < 5; ++i) {
        sum += amp * value_noise(p);
        p = rot * p * 1.93;
        amp *= 0.5;
    }
    return sum;
}

// --- the sky itself ------------------------------------------------------------------------------
vec3 sky_radiance(vec3 dir) {
    // Gradient: saturated blue overhead easing to a pale, slightly warm horizon. pow() on the
    // upward component puts the transition where the eye expects it rather than halfway up.
    float up = clamp(dir.y, -1.0, 1.0);
    float t = pow(clamp(up, 0.0, 1.0), 0.42);
    vec3 col = mix(sky.horizon.rgb, sky.zenith.rgb, t);

    // Below the horizon the "sky" is whatever ground haze there is -- darkened horizon colour. The
    // scene's own geometry covers this in practice; it matters at the edges of a landscape.
    if (up < 0.0) {
        col = mix(sky.horizon.rgb * sky.horizon.a, col, clamp(1.0 + up * 6.0, 0.0, 1.0));
    }

    // Forward scattering: a broad glow around the sun, brightest near it. This is the one term that
    // makes an analytic sky read as air rather than as a gradient.
    float mu = clamp(dot(dir, sky.sun_dir.xyz), -1.0, 1.0);
    float glow = pow(max(mu, 0.0), 8.0) * 0.35 + pow(max(mu, 0.0), 2.0) * 0.10;
    col += sky.sun_radiance.rgb * glow;

    // The sun disc. Angular radius comes in as a parameter (the real sun is ~0.0047 rad); the outer
    // few percent are feathered so the edge does not alias into a hexagon at low resolution.
    float ang = acos(mu);
    float r = sky.sun_radiance.a;
    float disc = 1.0 - smoothstep(r * 0.85, r * 1.15, ang);
    col += sky.sun_radiance.rgb * disc * 12.0;

    return col * sky.zenith.a;
}

// Cloud cover along `dir`, returned as (coverage, lit) so the caller can composite.
vec2 clouds(vec3 dir) {
    if (sky.wind.w < 0.5 || dir.y <= 0.02) {
        return vec2(0.0); // disabled, or looking at or below the horizon where the slab is edge-on
    }
    // Where the ray pierces a flat slab at cloud altitude. Near the horizon this stretches without
    // bound, which is what gives the correct perspective compression of a real cloud deck.
    float dist = sky.cloud.z / dir.y;
    vec2 p = (sky.camera_pos.xz + dir.xz * dist) * sky.cloud.w + sky.wind.xy;

    float n = fbm(p);
    // Coverage thresholds the noise; sharpness controls how hard the cloud edge is. Fade the whole
    // layer out toward the horizon so the slab's own edge is never visible as a hard line.
    float cov = smoothstep(1.0 - sky.cloud.x, 1.0 - sky.cloud.x + sky.wind.z, n);
    cov *= smoothstep(0.02, 0.28, dir.y);
    cov = clamp(cov * sky.cloud.y, 0.0, 1.0);

    // A second, offset fBm sample stands in for self-shadowing: where the density toward the sun is
    // higher, the cloud is darker. Not a light transport calculation -- a shading trick that reads.
    float toward_sun = fbm(p + sky.sun_dir.xz * 2.4);
    float lit = clamp(1.15 - (toward_sun - n) * 1.8, 0.35, 1.25);
    return vec2(cov, lit);
}

void main() {
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const vec3 scene = texelFetch(scene_color, px, 0).rgb;
    const float depth = texelFetch(scene_depth, px, 0).r;

    // Anything the forward pass drew keeps its colour EXACTLY. The sky only fills what the depth
    // buffer says is still at the far plane -- so with an opaque scene this pass is a no-op on every
    // shaded pixel, which is what makes "sky on" safe to compare against "sky off".
    if (depth < 1.0) {
        out_color = vec4(scene, 1.0);
        return;
    }

    // Reconstruct the world-space view ray. z = 1 is the far plane in Vulkan NDC; w-divide then
    // subtract the camera to get a direction. Doing this from the inverse view-projection means the
    // ray is correct for any projection the camera happens to have, including the editor's.
    vec2 ndc = (gl_FragCoord.xy / vec2(textureSize(scene_color, 0))) * 2.0 - 1.0;
    vec4 far = sky.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - sky.camera_pos.xyz);

    vec3 col = sky_radiance(dir);

    vec2 c = clouds(dir);
    if (c.x > 0.0) {
        // Cloud colour is white scaled by the sun, shaded by the pseudo-self-shadow term, with a
        // little of the sky's own colour mixed into the shadowed side so they sit in the air rather
        // than on top of it.
        vec3 cloud_col = sky.sun_radiance.rgb * c.y * 0.85 + sky.zenith.rgb * 0.25;
        col = mix(col, cloud_col, c.x);
    }

    out_color = vec4(col, 1.0);
}
