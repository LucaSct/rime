// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The FX particle pass, fragment stage (m13.1a, ADR-0035 §5 fx1a).
//
// A SOFT DISC, NOT A TEXTURED SPRITE. v1 has no FX texture pipeline — cooking, streaming and
// binding a sprite atlas is its own brick — so the billboard's shape is computed: a radial falloff
// that is 1 at the centre and 0 at the inscribed circle, squared so the edge is soft rather than a
// visible rim. Two multiplies, no sampler, no atlas, and it looks like dust rather than like a
// square. When the atlas arrives it replaces exactly this expression.
//
// DISCARDING OUTSIDE THE DISC is not an optimization. The quad's corners sit at radius √2, well
// outside the disc, and additive blending has no zero: a fragment whose falloff is 0 still costs a
// blend and, at the corners of a few thousand overlapping quads, that is measurable for pixels that
// contribute nothing. Cutting them is cheaper than blending them.
#version 450

layout(location = 0) in vec2 in_uv;    // [-1,1]², the billboard's own space
layout(location = 1) in vec4 in_color; // rgb = linear radiance, a = fade

layout(location = 0) out vec4 out_color;

void main() {
    const float r2 = dot(in_uv, in_uv);
    if (r2 >= 1.0) {
        discard;
    }
    const float falloff = 1.0 - r2;
    const float intensity = falloff * falloff * in_color.a;

    // ADDITIVE, so alpha is folded into rgb rather than left for the blend to apply. The pipeline
    // blends ONE·src + ONE·dst; premultiplying here is what makes `a` behave as a fade instead of
    // being ignored, and it is why the alpha channel written below is 0 — an additive pass must
    // not disturb the target's alpha.
    out_color = vec4(in_color.rgb * intensity, 0.0);
}
