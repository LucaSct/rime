// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// HUD overlay fragment stage (m13.3b): solid fills and distance-field glyphs from one pipeline.
//
// THE GLYPH PATH is Green (2007). The atlas stores, per texel, the signed distance to the nearest
// glyph edge remapped into 0..1 with 0.5 AT the edge. Thresholding at 0.5 would give a hard,
// aliased edge; instead we `smoothstep` across a band whose width is derived from `fwidth` — the
// screen-space rate of change of the sampled distance. That is what makes one small atlas look
// correct at any size: when text is drawn large the field changes slowly across a pixel and the
// band is narrow; when it is small the band widens and the glyph softens instead of shimmering.
//
// A fixed band would be wrong in both directions at once, which is the usual way this is got wrong.
#version 450

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_mode;

layout(location = 0) out vec4 out_color;

void main() {
    if (v_mode < 0.5) {
        out_color = v_color; // a panel: flat fill, alpha straight from the colour
        return;
    }
    const float d = texture(atlas, v_uv).r;
    // Half the per-pixel change in the field, clamped so a degenerate derivative (a fully
    // magnified glyph, or a driver returning 0) still yields a usable edge rather than a hard one.
    const float w = clamp(0.5 * fwidth(d), 0.001, 0.4);
    const float coverage = smoothstep(0.5 - w, 0.5 + w, d);
    out_color = vec4(v_color.rgb, v_color.a * coverage);
}
