// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The m19.7b ALPHA-IS-LINEAR probe (ADR-0066 addendum). Paired with the engine's fullscreen.vert
// over a target the size of the texture, so pixel (x, y)'s centre is texel (x, y)'s centre. It
// samples the layer texture through a sampler configured exactly like TerrainPass's layer sampler
// and reports what the SHADER sees, losslessly: the red channel's and the alpha channel's f32 bits.
//
// textureLod(…, 0.0) rather than texture(): at a 1:1 footprint the implicit LOD is log2(1) = 0
// only up to the device's derivative precision, and a trilinear sampler would blend a sliver of
// mip 1 into anything a hair above 0. The claim under test is the FORMAT's decode, not LOD
// selection, so the LOD is pinned and the filter weights are exactly (1, 0) at a texel centre.
#version 450

layout(set = 0, binding = 0) uniform sampler2D layer_texture;

layout(location = 0) out uvec2 out_bits;

void main() {
    const vec2 uv = gl_FragCoord.xy / vec2(textureSize(layer_texture, 0));
    const vec4 t = textureLod(layer_texture, uv, 0.0);
    out_bits = uvec2(floatBitsToUint(t.r), floatBitsToUint(t.a));
}
