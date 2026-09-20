// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Reading the sky's SH irradiance (m17.7b) — the evaluation half of sky_sh.comp.
//
// The projection lives in sky_sh.comp and the evaluation lives here, and the ONE thing that has to
// be true is that both use the same basis polynomials in the same order. They are written out
// identically in both files for exactly that reason; a shared basis function would be nicer, but
// the projection accumulates into nine separate registers and the evaluation collapses to a single
// dot, so there is no shape that serves both without making one of them worse. sky_lighting_test
// asserts the pair agrees on numbers computed independently on the CPU, which is the real guard.
//
// The stored coefficients are ALREADY convolved with the cosine lobe and divided by pi (see
// sky_sh.comp), so what comes out of sky_sh_irradiance() is the outgoing radiance of a white
// Lambertian surface facing `n` — the same quantity, in the same units, that
// SceneRenderer::ambient_ stood in for as a single constant. That is what makes this a drop-in
// replacement rather than a re-tuning: under a uniform sky of radiance L it returns exactly L.
//
// The includer must define SKY_SH_BINDING first, because the binding differs per pipeline and a
// header has no business choosing a descriptor slot in someone else's set.
#ifndef RIME_SKY_SH_EVAL_GLSL
#define RIME_SKY_SH_EVAL_GLSL

#ifndef SKY_SH_BINDING
#error "define SKY_SH_BINDING before including sky_sh_eval.glsl"
#endif

layout(std430, set = 0, binding = SKY_SH_BINDING) readonly buffer SkyShRead {
    vec4 coeff[10]; // [0..8].xyz = the nine coefficients, [9].x = 1.0 when a live sky wrote them
}
sky_sh;

// False for the sky-off placeholder, which is an all-zero buffer. The caller keeps its previous
// constant-ambient path behind this, which is ADR-0032 §11's rule: a switched-off feature leaves
// the frame byte-identical, and the shader's own branch is what guarantees it — not the contents
// of whatever dummy resource is bound.
bool sky_sh_enabled() {
    return sky_sh.coeff[9].x != 0.0;
}

// The diffuse radiance leaving a white Lambertian surface whose world-space normal is `n`.
vec3 sky_sh_irradiance(vec3 n) {
    const vec3 d = normalize(n);
    return sky_sh.coeff[0].xyz * 0.2820948 +
           sky_sh.coeff[1].xyz * (0.4886025 * d.y) +
           sky_sh.coeff[2].xyz * (0.4886025 * d.z) +
           sky_sh.coeff[3].xyz * (0.4886025 * d.x) +
           sky_sh.coeff[4].xyz * (1.0925484 * d.x * d.y) +
           sky_sh.coeff[5].xyz * (1.0925484 * d.y * d.z) +
           sky_sh.coeff[6].xyz * (0.3153916 * (3.0 * d.z * d.z - 1.0)) +
           sky_sh.coeff[7].xyz * (1.0925484 * d.x * d.z) +
           sky_sh.coeff[8].xyz * (0.5462742 * (d.x * d.x - d.y * d.y));
}

#endif // RIME_SKY_SH_EVAL_GLSL
