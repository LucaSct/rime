// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The depth pre-pass fragment shader for ALPHA-MASKED materials (m16.4).
//
// The pre-pass has had no fragment shader at all since M5.6, which is correct and fast for opaque
// geometry: rasterization runs only for the fixed-function depth test/write. But a masked material
// discards fragments, and a pre-pass that cannot discard writes depth for texels the forward pass
// then throws away — so everything BEHIND a leaf's transparent hole fails the forward pass's
// CompareOp::Equal test and is never shaded. The hole renders as clear colour: a black cutout in
// front of a lit wall.
//
// That is a PRIMARY-VIEW bug, not only a shadow one, and the pre-pass is on by default in every
// production call site. m15.6a's proof could not see it because it renders with the pre-pass OFF —
// the one configuration nothing ships.
//
// The test below is byte-for-byte the one in pbr_forward.frag, and must stay that way: if the two
// disagree about which fragments survive, the pre-pass lays depth the forward pass rejects, which
// is the same z-fighting failure `invariant gl_Position` exists to prevent.
#version 450

layout(location = 0) in vec2 v_uv;

layout(std140, set = 0, binding = 1) uniform DrawUniforms {
    mat4 model;
    mat4 normal_matrix;
    vec4 base_color;
    vec4 params;
    vec4 emissive; // w = alpha cutoff; 0 = never mask (m15.6)
} draw;

layout(set = 0, binding = 2) uniform sampler2D base_color_tex;

void main() {
    vec4 base_sample = texture(base_color_tex, v_uv);
    float base_alpha = draw.base_color.a * base_sample.a;
    if (draw.emissive.w > 0.0 && base_alpha < draw.emissive.w) {
        discard;
    }
    // No colour output and no explicit depth write: surviving fragments take the fixed-function
    // depth exactly as the unmasked variant does, so the two agree on every pixel that is not
    // discarded.
}
