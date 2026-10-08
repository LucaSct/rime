// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// GGX importance sampling -- the shared half of the two split-sum bakes (ADR-0078 section 2).
// sky_specular_prefilter.comp and sky_specular_dfg.comp both estimate an integral against the GGX
// lobe, and both draw their samples here so they cannot disagree about which lobe they mean.
// Binding-free on purpose, like brdf.glsl: it declares no uniform, so any shader can include it.
//
// WHY IMPORTANCE SAMPLING. A smooth surface's lobe is a few degrees wide. Throwing uniformly random
// directions at it would spend nearly every sample where the BRDF is ~0 and need millions to see
// the spike. Importance sampling instead draws directions with probability proportional to the
// thing we are integrating against, so every sample lands where the integrand is large; the
// estimator then divides by that probability (the "pdf") to stay unbiased. For GGX the pdf of a
// half-vector h is D(h)*cos(theta_h), and its inverse CDF has a closed form, which is the
// `cos_theta` line below.
#ifndef RIME_SKY_SPECULAR_SAMPLING_GLSL
#define RIME_SKY_SPECULAR_SAMPLING_GLSL

#include "brdf.glsl" // kPi, d_ggx, v_smith_ggx

// Van der Corput radical inverse: mirror the bits of `bits` about the binary point. Paired with
// i/N it is the Hammersley point set -- a LOW-DISCREPANCY sequence, i.e. points spread far more
// evenly over the unit square than random ones, so N samples integrate a smooth function with
// error ~1/N rather than the 1/sqrt(N) of random sampling. It is also DETERMINISTIC: the same
// texel gets the same samples every bake, so a re-bake of an unchanged sky is bit-identical and
// there is no noise to shimmer under temporal accumulation.
float radical_inverse_vdc(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 2^32
}

// The i-th of n Hammersley points. The half-sample offset on the first axis keeps every point
// strictly inside (0, 1), so no sample ever lands exactly on the pole of the mapping below.
vec2 hammersley(uint i, uint n) {
    return vec2((float(i) + 0.5) / float(n), radical_inverse_vdc(i));
}

// A GGX-distributed HALF-VECTOR in tangent space (+z is the surface normal), from a uniform point
// xi in [0,1)^2. `alpha` is the GGX width, roughness squared. Azimuth is uniform; the polar angle
// inverts the GGX cumulative distribution:
//     cos^2(theta) = (1 - xi.y) / (1 + (alpha^2 - 1) xi.y).
vec3 importance_sample_ggx_half(vec2 xi, float alpha) {
    const float phi = 2.0 * kPi * xi.x;
    const float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    const float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    return vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
}

// Rotate a tangent-space vector into the world frame whose +z is `n`. Any orthonormal frame about n
// works: the GGX lobe is isotropic, so which tangent we call "x" cannot change the answer.
vec3 tangent_to_world(vec3 v, vec3 n) {
    const vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tx = normalize(cross(up, n));
    const vec3 ty = cross(n, tx);
    return tx * v.x + ty * v.y + n * v.z;
}

#endif // RIME_SKY_SPECULAR_SAMPLING_GLSL
