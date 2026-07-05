// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Tonemap + display encode (M5.6): the last stop between scene RADIANCE and a displayable image.
// Two distinct jobs, in the only order that works, both derived in docs/math/pbr.md §display:
//
//   1. TONEMAP — compress unbounded linear radiance into [0,1]. We use Krzysztof Narkowicz's
//      rational fit of the ACES filmic curve: a gentle toe (blacks keep contrast), a long
//      shoulder (highlights roll off instead of clipping to flat white), and film-like desaturation
//      of very bright values. Simple Reinhard (x/(1+x)) is the pedagogical baseline the doc
//      derives first; the ACES fit is what we ship because its shoulder treats HDR highlights —
//      the whole point of this pipeline — far more gracefully.
//
//   2. sRGB-ENCODE — apply the display transfer function. Monitors expect sRGB-encoded values;
//      writing linear values to an Unorm target makes everything mid-dark look wrong. Encoding in
//      the shader (rather than letting an Srgb-format target do it in hardware) keeps the choice
//      visible and this pass usable with any Unorm target; a swapchain-facing present pass can
//      switch to the free hardware encode later.
//
// The pass locates itself with gl_FragCoord and texelFetch — output pixel (x, y) reads exactly
// HDR texel (x, y), the bound sampler's filtering bypassed; no uv plumbing, no half-texel
// headaches (the sharper cousin of the rhi sampler-copy proof idiom).
#version 450

layout(set = 0, binding = 0) uniform sampler2D hdr_input;

layout(location = 0) out vec4 out_color;

// ACES filmic fit (Narkowicz 2015, "ACES Filmic Tone Mapping Curve"). Input: linear radiance,
// exposure already applied (we have no exposure system yet — M10 brings one with real units).
vec3 tonemap_aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// The piecewise sRGB opto-electronic transfer function (IEC 61966-2-1): linear below 0.0031308,
// a 1/2.4 power curve above. Not a plain gamma 2.2 — the linear toe avoids infinite slope at 0.
vec3 srgb_encode(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    vec3 hdr = texelFetch(hdr_input, ivec2(gl_FragCoord.xy), 0).rgb;
    out_color = vec4(srgb_encode(tonemap_aces(hdr)), 1.0);
}
