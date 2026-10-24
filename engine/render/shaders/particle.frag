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
layout(binding = 1) uniform sampler2D scene_depth;
layout(push_constant) uniform Pc {
    mat4 view_proj;
    vec4 camera_right;
    vec4 camera_up;
    vec4 depth_params;
} pc;

// Depth-difference soft particles replace the sharp billboard/geometry intersection with a
// metric ramp. Perspective depth is nonlinear, so reconstruct both view distances before
// subtracting; fading raw depth would shrink the fade with camera distance. Smoke deliberately
// keeps its established opacity. Nearest texel fetch avoids blending foreground/background depth.
float intersection_fade() {
    float scene = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
    if (scene >= 1.0) return 1.0;
    float gap;
    if (pc.depth_params.w > 0.5) {
        float scene_distance = pc.depth_params.y / (scene - pc.depth_params.x);
        float particle_distance = 1.0 / gl_FragCoord.w;
        gap = scene_distance - particle_distance;
    } else {
        gap = (scene - gl_FragCoord.z) * pc.depth_params.y;
    }
    return smoothstep(0.0, pc.depth_params.z, gap);
}

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
    if (in_mode == 3) {
        // Teardrop core: wide at the fuel, narrowing to a tip. The boundary is eroded by two
        // octaves of noise that scroll with the sprite's own animation phase, so the outline is
        // ragged rather than elliptical. Heat (the pale yellow heart) is confined to the lower
        // third; above it the sprite is orange, so it reads as a flame body, not a white puff.
        const float height = in_uv.y * 0.5 + 0.5;
        const vec2 flow = vec2(in_uv.x * 2.6, in_uv.y * 3.4);
        const float n1 = smooth_noise(flow + in_noise_origin);
        const float n2 = smooth_noise(flow * 2.3 + in_noise_origin.yx + vec2(5.1, 1.7));
        const float noise = 0.65 * n1 + 0.35 * n2;
        const float bend = (n1 - 0.5) * height * 0.55;
        const float taper = pow(clamp(1.0 - height * 0.92, 0.0, 1.0), 0.85);
        const float width = max(taper * mix(0.62, 1.15, noise), 0.02);
        const float across = abs(in_uv.x - bend) / width;
        // Erode: raising the threshold with height lets noise bite the shoulders and tip.
        const float erode = across + (0.55 - noise) * (0.35 + 0.9 * height);
        const float body = (1.0 - smoothstep(0.35, 1.0, erode)) *
                           smoothstep(0.0, 0.10, height) *
                           (1.0 - smoothstep(0.78, 1.0, height + (noise - 0.5) * 0.25));
        const float heart = (1.0 - smoothstep(0.12, 0.40, height)) *
                            (1.0 - smoothstep(0.2, 0.9, across));
        const float mid = (1.0 - smoothstep(0.25, 0.75, height));
        const vec3 heat = mix(mix(vec3(0.95, 0.30, 0.03), vec3(1.0, 0.55, 0.09), mid),
                              vec3(1.0, 0.78, 0.34), heart);
        out_color = vec4(in_color.rgb * heat * body * mix(0.7, 1.0, noise) *
                         in_color.a * intersection_fade(), 0.0);
        return;
    }
    if (in_mode == 2) {
        const float soft = intersection_fade();
        // Tiny fire particles are embers: compact bright discs that cool in the CPU colour
        // ramp and wink out with their own age. The size tag preserves the billboard ABI.
        if (in_half_size < 0.04) {
            const float r2 = dot(in_uv, in_uv);
            if (r2 >= 1.0) discard;
            const float glow = (1.0 - r2) * (1.0 - r2) * in_color.a;
            out_color = vec4(in_color.rgb * glow * soft, 0.0);
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
        const float width = (0.85 * pow(1.0 - height, 0.95) + 0.01) *
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
        const float core = pow(max(1.0 - across, 0.0), 0.55) *
                           (1.0 - smoothstep(0.28, 0.95, height)) * mix(0.78, 1.0, filament);
        const vec3 heat = mix(mix(vec3(0.85, 0.09, 0.008),
                                   vec3(1.0, 0.58, 0.07),
                                   smoothstep(0.0, 0.45, core)),
                              vec3(1.0, 0.94, 0.72),
                              smoothstep(0.28, 0.80, core));
        out_color = vec4(in_color.rgb * heat * opacity * soft, 0.0);
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
