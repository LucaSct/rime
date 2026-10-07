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

// ── GEOMETRIC SPECULAR ANTI-ALIASING (ADR-0078 step 1a) ───────────────────────────────────────
//
// THE PROBLEM. D(n·h) assumes the surface has ONE shading normal per pixel. It does not: a pixel
// covers a patch of surface, and on a normal-mapped or finely curved patch the normals inside that
// footprint fan out. A smooth metal (alpha ~ 0.002) under a point-like light lights only the sliver
// of normals that happen to align with the half-vector -- a highlight far smaller than a pixel. We
// shade ONE normal per pixel, so whether that sliver is caught depends on where the pixel centre
// falls, and that changes every frame as the camera moves. The highlight flickers at full contrast.
//
// THE FIX (Kaplanyan, Hill, Hoffman & Pettineo, "Filtering Distributions of Normals for Shading
// Antialiasing", HPG 2016; the cheap isotropic form is Tokuyoshi & Kaplanyan, "Improved Geometric
// Specular Antialiasing", I3D 2019, and the one Filament ships). The pixel does not see one normal,
// it sees a DISTRIBUTION of them. Convolving a GGX lobe of width alpha with that footprint's spread
// of normals gives, to first order, a WIDER GGX lobe. So: measure the spread from how fast the
// shading normal changes across the screen (dFdx(n), dFdy(n): the hardware already differences
// neighbouring pixels of the 2x2 quad), turn it into extra lobe width, and use the wider lobe. The
// highlight then covers the pixel it really occupies instead of hopping between neighbours.
//
// WHY THE SPREAD IS ADDED IN alpha^2 AND NOT IN alpha. Convolution adds VARIANCES, not standard
// deviations -- the same reason two independent noise sources of width a and b combine to
// sqrt(a^2 + b^2), not a + b. alpha^2 is the quantity that behaves like a variance of the
// microfacet slope, and the screen footprint's slope variance is (a constant times) the squared
// derivative of the normal. Adding to alpha directly over-widens a smooth surface by a factor that
// looks almost right at low roughness and is wrong everywhere, which is the worst kind of wrong.
// So we add in alpha^2 and take the square root to hand shade_light the alpha it expects.
//
// WHY THE CLAMP. At a sphere's silhouette n turns fast enough that the raw variance would push alpha
// to 1 and the rim would read as chalk. The paper caps the added alpha^2 at 0.18 (their suggested
// kernel-width limit); we keep it, because it is what makes the filter safe to leave always on.
//
// WHY BEFORE ANY TEMPORAL RESOLVE, NOT AFTER. A temporal resolve (TAA) averages frames and clamps
// each history sample to its neighbourhood. Fed an aliased highlight -- a one-pixel, full-contrast
// spike that moves every frame -- it either clamps the spike away (the highlight dims or vanishes)
// or smears it into a streak. By then the information is gone: the signal has become high-variance
// and no amount of averaging restores the lobe that should have been integrated over the pixel.
// Widening the lobe at the SOURCE is a different operation, not a smaller version of TAA, so it has
// to run first. (Tokuyoshi & Kaplanyan and Kaplanyan et al. both say TAA alone does not cure
// specular sparkle.)
//
// CONTRACT. `n` is the normalized shading normal the fragment shades with -- the PERTURBED normal,
// because normal-map detail is the dominant source of sub-pixel normal variance, and filtering only
// the interpolated vertex normal would miss exactly the sparkle this exists for. `alpha` is GGX
// alpha (perceptual roughness squared). Returns alpha' >= alpha. On a flat surface n is constant, so
// dFdx(n) = dFdy(n) = 0 and alpha' == alpha exactly: the filter is inert where it must be.
//
// dFdx/dFdy are only defined in a fragment shader, and only meaningful in UNIFORM control flow:
// call this once, near the top of main(), never inside a light loop or a branch. It is NOT called
// from a fullscreen pass over a G-buffer (ssr_resolve.frag): there, the screen-space step in the
// normal crosses object silhouettes rather than measuring curvature.
float filter_specular_alpha(vec3 n, float alpha) {
    const float kScreenSpaceVariance = 0.15915494; // 1 / (2*pi): T&K 2019, the footprint's variance
    const float kClampThreshold = 0.18;            // cap on the added alpha^2 (kernel width limit)

    const vec3 dndx = dFdx(n);
    const vec3 dndy = dFdy(n);
    const float variance = kScreenSpaceVariance * (dot(dndx, dndx) + dot(dndy, dndy));
    const float kernel_alpha2 = min(2.0 * variance, kClampThreshold);
    const float filtered_alpha2 = clamp(alpha * alpha + kernel_alpha2, 0.0, 1.0);
    return sqrt(filtered_alpha2);
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
    // Lambert's albedo/pi: the 1/pi is the normalisation that makes a white Lambertian surface
    // reflect exactly the energy it receives and no more; dropping it is the single most common
    // way a renderer ends up pi times too bright.
    vec3 diffuse = kd * albedo / kPi;

    return (diffuse + specular) * radiance * n_dot_l;
}

// ── THE ENVIRONMENT BRDF, ANALYTICALLY ────────────────────────────────────────────────────────
//
// Lighting a surface by a whole environment means integrating the GGX lobe against it. The
// "split-sum" approximation (Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013) factors that
// integral into (the environment averaged over the lobe) x (the BRDF integrated against a WHITE
// environment). The second factor depends only on f0, roughness and n.v, and is linear in f0:
// f0 * A + B. Unreal stores A and B in a 2-D lookup texture; Karis' mobile follow-up ("Physically
// Based Shading on Mobile", 2014) fits them with the handful of terms below — "EnvBRDFApprox". It is
// within a few percent of the LUT, and it costs no texture, no bake and no binding, which is why
// terrain (m19.6) and the forward PBR pass (m19.6b) use it rather than growing a second lookup
// table. `roughness` is PERCEPTUAL roughness.
vec3 env_brdf_approx(vec3 f0, float roughness, float n_dot_v) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

#endif // RIME_BRDF_GLSL
