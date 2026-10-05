// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The shared Cook-Torrance BRDF (m19.5) -- GGX distribution, height-correlated Smith visibility,
// Schlick Fresnel and the per-light shade_light() that sums them. Moved verbatim out of
// pbr_forward.frag so terrain.frag shades with the SAME BRDF instead of a fourth copy; the
// derivation is in docs/math/pbr.md. Binding-free on purpose: it declares no uniforms, so any
// shader can include it without inheriting a descriptor it does not own.
#ifndef RIME_BRDF_GLSL
#define RIME_BRDF_GLSL

const float kPi = 3.14159265358979;

// D — GGX / Trowbridge-Reitz normal distribution: the statistical concentration of microfacet
// normals around n. α = roughness² (Disney's perceptual remap: even slider steps look even).
// GGX's fat tail is why its highlights have the soft halo real materials show.
float d_ggx(float n_dot_h, float alpha) {
    float a2 = alpha * alpha;
    float t = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * t * t);
}

// V — height-correlated Smith visibility: what fraction of microfacets both the light and the
// eye actually see (masking + shadowing), divided by the 4·NoV·NoL projection Jacobian of the
// half-vector parameterization. Folding the two together (Heitz 2014) is numerically kinder than
// computing G alone and dividing — no 0/0 at grazing angles.
float v_smith_ggx(float n_dot_v, float n_dot_l, float alpha) {
    float a2 = alpha * alpha;
    float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// F — Fresnel-Schlick: reflectance grows from f0 (head-on) to 1 (grazing) as the fifth power of
// the complement — the cheap curve that fits the full Fresnel equations to within a percent. f0
// is 0.04 for dielectrics (glass/plastic/wood all sit near 4%) and the base color for metals.
vec3 f_schlick(float v_dot_h, vec3 f0) {
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

// One punctual light's contribution: BRDF × incident radiance × the geometry cosine. `l` points
// from the surface TOWARD the light; `radiance` is what arrives at this point (falloff already
// applied for point lights).
vec3 shade_light(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, float metallic, float alpha) {
    float n_dot_l = dot(n, l);
    if (n_dot_l <= 0.0)
        return vec3(0.0); // the light is behind the surface — no transport, and no negative light
    vec3 h = normalize(v + l);
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(v, h), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = f_schlick(v_dot_h, f0);
    vec3 specular = d_ggx(n_dot_h, alpha) * v_smith_ggx(n_dot_v, n_dot_l, alpha) * fresnel;

    // Energy split: what Fresnel reflected specularly cannot ALSO scatter diffusely, and metals
    // have no diffuse at all (their "color" is the F0 of the specular lobe).
    vec3 kd = (vec3(1.0) - fresnel) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / kPi;

    return (diffuse + specular) * radiance * n_dot_l;
}

#endif // RIME_BRDF_GLSL
