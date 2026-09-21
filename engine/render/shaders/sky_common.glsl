// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky's shared body (m17.7a) -- the SkyParams block, the value-noise/fBm basis, sky_radiance()
// and the cloud layer, in ONE place so more than one shader can evaluate the same sky.
//
// WHY THIS FILE EXISTS. ADR-0040 Section 2 fixes the seam at `sky_radiance(vec3 dir)` and promises
// that a physical atmosphere replaces its BODY without any other file moving. That promise only
// holds while there is exactly one body. m17.7 gives the sky two more readers -- a sky-view LUT
// that bakes it, and an SH projection that integrates it -- and copying the function into each
// would quietly retire the promise on the day it was first needed. ssr_resolve.frag's own comment
// records what that costs: its four DDGI functions are "a verbatim COPY of the forward shader's
// (no shader-include mechanism exists)". Now one does.
//
// Anything that reads the SkyParams UBO belongs here. Anything binding-free and purely geometric
// (the sky-view LUT's direction<->texel mapping) deliberately does NOT, so a shader can share the
// mapping without also inheriting a uniform block at a binding it does not own.
#ifndef RIME_SKY_COMMON_GLSL
#define RIME_SKY_COMMON_GLSL

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
// `disc_scale` scales the sun's OWN DISC, and it is 1.0 for the background picture and 0.0 for
// anything that LIGHTS the scene.
//
// This is not a tuning knob, it is a double-counting guard (m17.7b). The sun's direct contribution
// already reaches every shaded pixel as the world's first DirectionalLight -- which is the same
// light this sky couples its disc to, by ADR-0040 Section 4. So a sky-derived ambient that also
// carried the disc would add the sun to the frame a SECOND time, and the brighter the sun the worse
// the error. The forward-scatter glow below is a different matter and is deliberately KEPT in both:
// it is light the atmosphere scattered out of the beam, which no DirectionalLight accounts for.
//
// The disc is also the one term a 192x108 sky-view LUT cannot represent -- it subtends ~0.7 deg
// against a ~1.8 deg texel -- so excluding it from the baked path is what the resolution wanted
// anyway. Hillaire keeps the disc analytic in the final pass for exactly this reason.
vec3 sky_radiance_ex(vec3 dir, float disc_scale) {
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
    col += sky.sun_radiance.rgb * disc * 12.0 * disc_scale;

    return col * sky.zenith.a;
}

// The seam ADR-0040 Section 2 names, unchanged in signature and meaning: the authored sky the
// background pass SEEes.  m17.7d deliberately leaves this full-resolution art control alone while
// replacing the lower-frequency sky-view/SH lighting body below; sampling that LUT in the final
// composite is a later presentation change, not an accidental consequence of changing the light.
vec3 sky_radiance(vec3 dir) {
    return sky_radiance_ex(dir, 1.0);
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

// The WHOLE sky along `dir` -- the gradient, the sun, and the cloud layer composited over them.
//
// This is the function that answers "what radiance arrives from this direction", and it exists so
// that the three things which need that answer cannot disagree: the background composite paints it
// per pixel, the sky-view LUT bakes it for the rays that miss the screen, and the SH projection
// integrates it into the irradiance the forward pass shades with. Before m17.7b only the composite
// existed, and the cloud blend below lived inline in sky.frag's main().
vec3 sky_full_radiance_ex(vec3 dir, float disc_scale) {
    vec3 col = sky_radiance_ex(dir, disc_scale);

    vec2 c = clouds(dir);
    if (c.x > 0.0) {
        // Cloud colour is white scaled by the sun, shaded by the pseudo-self-shadow term, with a
        // little of the sky's own colour mixed into the shadowed side so they sit in the air rather
        // than on top of it.
        vec3 cloud_col = sky.sun_radiance.rgb * c.y * 0.85 + sky.zenith.rgb * 0.25;
        col = mix(col, cloud_col, c.x);
    }
    return col;
}

// What a pixel of background shows: the whole sky, sun disc included.
vec3 sky_full_radiance(vec3 dir) {
    return sky_full_radiance_ex(dir, 1.0);
}

// What the scene is LIT by from this direction.  m17.7b began with the authored sky above so the
// cache/resource shape could be proven independently; m17.7d replaces only this body with a
// compact spherical-atmosphere single-scattering integral.  The physical declarations are gated
// because sky.frag still draws the authored background and deliberately owns no atmosphere
// descriptors.  Both compute callers define the gate before including this file.
#ifdef RIME_SKY_PHYSICAL_LIGHTING

const float kAtmospherePi = 3.14159265358979;

// Distance to the forward intersection with a sphere.  The starting point is inside the top shell,
// so the positive root is the atmospheric exit even for a ray initially aimed toward the ground.
float atmosphere_distance_to_sphere(float radius, float mu, float sphere_radius) {
    const float d = radius * radius * (mu * mu - 1.0) + sphere_radius * sphere_radius;
    return max(0.0, -radius * mu + sqrt(max(d, 0.0)));
}

// A negative-or-zero answer means the ray misses the solid planet in its forward half. Keeping this
// separate from the texture lookup is important: linear filtering of an alpha=0 LUT texel would
// otherwise leak a little sun through the terminator.
float atmosphere_distance_to_ground(float radius, float mu, float planet_radius) {
    const float d = radius * radius * (mu * mu - 1.0) + planet_radius * planet_radius;
    return d >= 0.0 ? -radius * mu - sqrt(d) : -1.0;
}

vec3 atmosphere_sun_transmittance(vec3 p, vec3 sun_dir) {
    const float planet_radius = max(atmosphere.radii.x, 1.0);
    const float top_height = max(atmosphere.radii.y, 0.001);
    const float radius = length(p);
    const vec3 up = p / max(radius, 1e-4);
    const float mu = clamp(dot(up, sun_dir), -1.0, 1.0);
    if (atmosphere_distance_to_ground(radius, mu, planet_radius) > 0.0) {
        return vec3(0.0);
    }
    // The transmittance producer stores height as v^2, so sampling it needs the inverse sqrt warp.
    const float height = clamp(radius - planet_radius, 0.0, top_height);
    const vec2 uv = vec2(0.5 + 0.5 * mu, sqrt(height / top_height));
    const vec4 t = textureLod(transmittance_lut, uv, 0.0);
    return t.rgb * t.a;
}

vec3 physical_sky_lighting_radiance(vec3 direction) {
    // The frame has no planet-centre coordinate, so this first physical body fixes the observer at
    // a 2 m ground-level eye.  That makes the medium camera-independent, which is what the
    // persistent cache currently promises.  Altitude-dependent camera flight is a later parameter
    // and must join the bake key when it lands.
    const float planet_radius = max(atmosphere.radii.x, 1.0);
    const float top_radius = planet_radius + max(atmosphere.radii.y, 0.001);
    const vec3 dir = normalize(direction);
    const vec3 sun_dir = normalize(sky.sun_dir.xyz);
    const vec3 observer = vec3(0.0, planet_radius + 0.002, 0.0); // kilometres
    const float observer_radius = length(observer);
    const float view_mu = dot(observer / observer_radius, dir);
    const float top_distance = atmosphere_distance_to_sphere(observer_radius, view_mu, top_radius);
    const float ground_distance =
        atmosphere_distance_to_ground(observer_radius, view_mu, planet_radius);
    const float distance = min(top_distance, ground_distance > 0.0 ? ground_distance : top_distance);
    if (distance <= 0.0) {
        return vec3(0.0);
    }

    // Twenty-four midpoint segments, quadratically distributed toward the observer where density
    // changes fastest.  This is single scattering with a tabulated bounded ambient term, not a
    // complete Hillaire multiple-scattering closure (that producer remains deliberately modest).
    const int steps = 24;
    const float cos_theta = clamp(dot(dir, sun_dir), -1.0, 1.0);
    const float rayleigh_phase = 3.0 * (1.0 + cos_theta * cos_theta) / (16.0 * kAtmospherePi);
    const float mie_g = 0.8;
    const float mie_phase = (1.0 - mie_g * mie_g) /
                            (4.0 * kAtmospherePi *
                             pow(1.0 + mie_g * mie_g - 2.0 * mie_g * cos_theta, 1.5));
    vec3 view_transmittance = vec3(1.0);
    vec3 radiance = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        const float t0 = float(i) / float(steps);
        const float t1 = float(i + 1) / float(steps);
        const float s0 = distance * t0 * t0;
        const float s1 = distance * t1 * t1;
        const float ds = s1 - s0;
        const vec3 p = observer + dir * (0.5 * (s0 + s1));
        const float radius = length(p);
        const float height = max(0.0, radius - planet_radius);
        const float rayleigh_density = exp(-height / max(atmosphere.radii.z, 0.001));
        const float mie_density = exp(-height / max(atmosphere.radii.w, 0.001));
        const vec3 beta_r = atmosphere.rayleigh_scattering.rgb * rayleigh_density;
        const vec3 beta_m = atmosphere.mie_scattering.rgb * mie_density;
        const vec3 sigma_s = beta_r + beta_m;
        const vec3 sigma_t = sigma_s + atmosphere.mie_absorption.rgb * mie_density;
        const vec3 segment_t = exp(-sigma_t * ds);
        const vec3 segment_integral = mix(vec3(ds),
                                           (vec3(1.0) - segment_t) / sigma_t,
                                           greaterThan(sigma_t, vec3(1e-5)));
        const vec3 up = p / max(radius, 1e-4);
        const vec2 uv = vec2(0.5 + 0.5 * dot(up, sun_dir),
                             sqrt(clamp(height / max(atmosphere.radii.y, 0.001), 0.0, 1.0)));
        const vec3 sun_transmittance = atmosphere_sun_transmittance(p, sun_dir);
        const vec3 multiple = textureLod(multiple_scattering_lut, uv, 0.0).rgb;
        const vec3 source = sun_transmittance *
                                (beta_r * rayleigh_phase + beta_m * mie_phase) +
                            sigma_s * multiple;
        radiance += view_transmittance * source * segment_integral;
        view_transmittance *= segment_t;
    }

    // The physical tables use a unit-white solar source so they cache independently of art.  The
    // scene's authored sun colour and sky intensity are applied once, here, rather than baked in.
    // Scene lights use a compact author-facing unit around one, whereas radiometric solar
    // irradiance is orders of magnitude larger.  This fixed conversion is intentionally outside
    // the cached medium: it maps that existing scene unit into the physical integral without
    // smuggling an authored sun value into either LUT producer.
    const float scene_solar_scale = 100.0;
    const vec3 illumination = max(sky.sun_radiance.rgb, vec3(0.0)) *
                              (max(sky.zenith.a, 0.0) * scene_solar_scale);
    vec3 col = radiance * illumination;
    const vec2 c = clouds(dir);
    if (c.x > 0.0) {
        const vec3 cloud_col = illumination * c.y * 0.85 + col * 0.25;
        col = mix(col, cloud_col, c.x);
    }
    return col;
}

vec3 sky_lighting_radiance(vec3 dir) {
    return physical_sky_lighting_radiance(dir);
}

#else

// sky.frag uses the authored background only in this brick.  Keeping the fallback makes the shared
// include stage-safe while the compute-only physical descriptors stay out of its pipeline layout.
vec3 sky_lighting_radiance(vec3 dir) {
    return sky_full_radiance_ex(dir, 0.0);
}

#endif // RIME_SKY_PHYSICAL_LIGHTING

#endif // RIME_SKY_COMMON_GLSL
