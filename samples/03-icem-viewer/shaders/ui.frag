// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the from-scratch UI overlay (E2). One pipeline draws both solid widgets and
// bitmap text. A solid quad carries u < 0, so its coverage is 1; a glyph quad samples the font atlas'
// red channel as coverage. The RHI has no alpha blending yet, so instead of blending we **alpha-test**:
// discard where coverage < 0.5. That keeps a panel opaque, lets the text show the panel through its
// glyph gaps, and gives crisp letters — an SDF atlas for smooth scaling is a later drop-in. See
// docs/math/ui-text-layout.md.
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;

void main() {
    float coverage = (v_uv.x < -0.5) ? 1.0 : texture(u_atlas, v_uv).r;
    if (coverage < 0.5) discard;
    out_color = v_color;
}
