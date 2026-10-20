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
layout(location = 2) flat in int in_mode;
layout(location = 3) flat in vec2 in_noise_origin;
layout(location = 4) flat in float in_half_size;

layout(location = 0) out vec4 out_color;

// Smooth value noise: interpolate deterministic lattice values with a cubic curve whose slope
// is zero at cell boundaries. Multiple scales break up opacity without hard pixel-sized speckle.
float hash_lattice(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float smooth_noise(vec2 p) {
    const vec2 cell = floor(p);
    const vec2 f = fract(p);
    const vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash_lattice(cell), hash_lattice(cell + vec2(1.0, 0.0)), u.x),
               mix(hash_lattice(cell + vec2(0.0, 1.0)),
                   hash_lattice(cell + vec2(1.0, 1.0)), u.x), u.y);
}

void main() {
    if (in_mode == 2) {
        // Tiny fire particles are embers: compact bright discs that cool in the CPU colour
        // ramp and wink out with their own age. The size tag preserves the billboard ABI.
        if (in_half_size < 0.04) {
            const float r2 = dot(in_uv, in_uv);
            if (r2 >= 1.0) discard;
            const float glow = (1.0 - r2) * (1.0 - r2) * in_color.a;
            out_color = vec4(in_color.rgb * glow, 0.0);
            return;
        }
        const float height = in_uv.y * 0.5 + 0.5;
        const vec2 flow = in_uv * vec2(2.7, 3.5) + in_noise_origin;
        const float coarse = smooth_noise(flow);
        const float detail = smooth_noise(flow * 2.1 + vec2(4.3, 7.1));
        // Domain warping bends the silhouette and advects bright internal filaments as each
        // mote rises. Tapering by height makes several overlapping sprites separate at their
        // tips; the smooth envelopes prevent a square top or hard cut at the base.
        const float bend = (coarse - 0.5) * height * 0.48 +
                           sin(height * 7.0 + in_noise_origin.x) * height * 0.11;
        const float width = (0.85 * pow(1.0 - height, 0.74) + 0.01) *
                            mix(0.72, 1.08, detail);
        const float across = abs(in_uv.x - bend) / max(width, 0.015);
        const float tongue = (1.0 - smoothstep(0.23, 1.0, across)) *
                             smoothstep(0.0, 0.10, height) *
                             (1.0 - smoothstep(0.89, 1.0, height));
        const float filament = smoothstep(0.25, 0.8, coarse * 0.7 + detail * 0.3);
        const float opacity = tongue * mix(0.48, 1.0, filament) *
                              clamp(in_color.a, 0.0, 1.0);
        if (opacity <= 0.0) {
            discard;
        }
        // A compact blackbody-inspired ramp reads from red cooling edges through orange and
        // yellow to the near-white core. Colour still fades to zero with each mote's lifetime.
        const float core = pow(max(1.0 - across, 0.0), 2.0) * (1.0 - height) * filament;
        const vec3 heat = mix(mix(vec3(0.75, 0.055, 0.008),
                                   vec3(1.0, 0.43, 0.025),
                                   smoothstep(0.0, 0.45, core)),
                              vec3(1.0, 0.94, 0.72),
                              smoothstep(0.45, 1.0, core));
        out_color = vec4(in_color.rgb * heat * opacity, 0.0);
        return;
    }

    const float r2 = dot(in_uv, in_uv);
    if (r2 >= 1.0) {
        discard;
    }
    const float falloff = 1.0 - r2;
    const float intensity = falloff * falloff * in_color.a;

    if (in_mode == 1) {
        // Straight alpha: the RHI blends src_alpha*rgb + (1-src_alpha)*dst.
        // RGB is caller-provided approximate incident lighting, not premultiplied here.
        const vec2 p = in_uv * 2.1 + in_noise_origin;
        const float detail = 0.57 * smooth_noise(p) +
                             0.29 * smooth_noise(p * 2.03 + vec2(4.1, 7.3)) +
                             0.14 * smooth_noise(p * 4.07 + vec2(9.2, 2.8));
        // Noise modulates density and the inner shoulder; the outer radial fade still reaches
        // exactly zero at the inscribed circle. This is not soft scene-depth intersection:
        // geometry intersections remain depth-tested billboard boundaries until that RHI brick.
        const float edge = 1.0 - smoothstep(0.22, 1.0, r2 + 0.18 * (detail - 0.5));
        const float density = mix(0.35, 1.0, detail) * edge * falloff;
        out_color = vec4(in_color.rgb, clamp(in_color.a, 0.0, 1.0) * density);
        return;
    }

    // ADDITIVE, so alpha is folded into rgb rather than left for the blend to apply. The pipeline
    // blends ONE·src + ONE·dst; premultiplying here is what makes `a` behave as a fade instead of
    // being ignored, and it is why the alpha channel written below is 0 — an additive pass must
    // not disturb the target's alpha.
    out_color = vec4(in_color.rgb * intensity, 0.0);
}
