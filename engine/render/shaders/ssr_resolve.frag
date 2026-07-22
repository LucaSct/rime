// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Screen-space reflections — the resolve pass (m10.7b, ADR-0032 §5). One fragment per pixel:
// reconstruct the surface from the depth buffer and the m10.7a G-buffer, reflect the view ray about
// the surface normal, and MARCH that ray through the depth buffer in view space. Where the ray
// crosses behind a depth sample within a thickness tolerance it has hit on-screen geometry — sample
// the frame's own colour there. Where it leaves the screen or finds nothing, fall back to the flat
// sky/ambient term (a DDGI-probe specular fallback is the named m10.7c follow-up). The reflection is
// added to the pixel's lit colour weighted by Fresnel and a roughness fade, and written to a second
// HDR target the tonemap then reads. docs/math/ssr.md derives the march, the thickness problem, and
// the artifacts v1 accepts.
//
// Why a FULLSCREEN RASTER pass and not a compute dispatch: v1's fixed linear march has nothing a
// compute shader gives it — no groupshared cooperation, no scatter — so a fullscreen fragment is the
// simpler realization, and it writes its result as an ordinary colour attachment the tonemap then
// samples (the exact forward→tonemap path every frame already exercises). Compute earns its keep at
// m10.7c, where a hi-Z march wants a shared depth pyramid and temporal accumulation wants to read and
// write history; both slot in behind this same SsrInputs/GpuSsrUniforms seam.
//
// v1 is a fixed-step LINEAR march (no hi-Z acceleration), full-res, no temporal accumulation — the
// spike-deferred choices in ADR-0032 §5. lavapipe renders exact depth, so a structural mirror-floor
// proof does not need those; a real GPU at m12.0 does, and they slot in behind this same interface.

#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_color; // the lit HDR frame (reflection source)
layout(set = 0, binding = 1) uniform sampler2D gbuffer;     // RG oct world normal, B roughness, A mask
layout(set = 0, binding = 2) uniform sampler2D scene_depth; // D32, Vulkan NDC z in [0,1]

layout(std140, set = 0, binding = 3) uniform SsrParams {
    mat4 proj;             // clip-from-view — projects a view-space march step to the screen
    mat4 inv_proj;         // view-from-clip — reconstructs a view position from a uv + depth
    mat4 view;             // world→view, to rotate the G-buffer's world normal into view space
    vec4 extent_near_far;  // xy = render size (px), z = near, w = far
    vec4 params;           // x = max_steps, y = thickness (view units), z = unused, w = max_distance
    vec4 ambient;          // rgb = the flat sky/ambient a missed ray reflects (matches the forward's)
} ssr;

layout(location = 0) out vec4 out_hdr; // scene_color + reflection (a second HDR target)

// Inverse of ddgi_oct_encode (pbr_forward_shadowed.frag) — the G-buffer stores the world normal this
// way; SSR only needs it back to rotate into view space, so decode without the final normalize's cost
// mattering (it is normalized in view space anyway).
vec3 oct_decode(vec2 e) {
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        v.xy = vec2((1.0 - abs(v.y)) * (v.x >= 0.0 ? 1.0 : -1.0),
                    (1.0 - abs(v.x)) * (v.y >= 0.0 ? 1.0 : -1.0));
    }
    return normalize(v);
}

// A uv (in [0,1], y-down to match the frame's own render) + an NDC depth back to a view-space
// position, through the inverse projection. The perspective divide is what makes it a POSITION and
// not just a homogeneous ray — reused for both the shaded pixel and every depth sample on the march.
vec3 view_pos(vec2 uv, float ndc_z) {
    vec4 clip = vec4(uv * 2.0 - 1.0, ndc_z, 1.0);
    vec4 p = ssr.inv_proj * clip;
    return p.xyz / p.w;
}

void main() {
    const vec2 extent = ssr.extent_near_far.xy;
    const vec2 uv = gl_FragCoord.xy / extent; // gl_FragCoord is pixel-centred, so this is (pix+0.5)/N

    const vec3 base = texture(scene_color, uv).rgb; // this pixel's own lit colour, always kept
    const vec4 gb = texture(gbuffer, uv);
    const float depth = texture(scene_depth, uv).r;

    // No geometry here (cleared G-buffer mask, or the far plane) → nothing reflects, pass the pixel
    // through unchanged. This is the branch that keeps the sky and background bit-identical.
    if (gb.a < 0.5 || depth >= 1.0) {
        out_hdr = vec4(base, 1.0);
        return;
    }

    const float roughness = gb.b;
    // Roughness fade: a mirror march only makes sense for smoothish surfaces; as roughness climbs the
    // single sharp sample is wrong and we fade SSR out (the blurred cone resolve is m10.7c). Above the
    // upper knob SSR contributes nothing — skip the whole march.
    const float rough_fade = 1.0 - smoothstep(0.25, 0.55, roughness);
    if (rough_fade <= 0.0) {
        out_hdr = vec4(base, 1.0);
        return;
    }

    const vec3 p_view = view_pos(uv, depth);
    const vec3 n_view = normalize((ssr.view * vec4(oct_decode(gb.rg), 0.0)).xyz);
    const vec3 v_dir = normalize(p_view);      // camera (origin) → surface
    const vec3 r_dir = reflect(v_dir, n_view); // the reflection ray, view space

    const int max_steps = int(ssr.params.x);
    const float thickness = ssr.params.y;
    const float near = ssr.extent_near_far.z;
    const float step_len = ssr.params.w / float(max_steps);

    vec3 refl = ssr.ambient.rgb; // miss default: the flat sky the forward pass also uses
    float edge = 1.0;
    bool hit = false;
    for (int i = 1; i <= max_steps && !hit; ++i) {
        const vec3 s_view = p_view + r_dir * (step_len * float(i));
        if (s_view.z > -near)
            break; // stepped in front of the near plane — off the top of the frustum
        const vec4 clip = ssr.proj * vec4(s_view, 1.0);
        if (clip.w <= 0.0)
            break;
        const vec2 s_uv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (any(lessThan(s_uv, vec2(0.0))) || any(greaterThan(s_uv, vec2(1.0))))
            break; // ran off the side of the screen — a miss, fall back
        const float s_depth = texture(scene_depth, s_uv).r;
        if (s_depth >= 1.0)
            continue; // that pixel is background (no surface to hit); keep marching
        const vec3 scene_view = view_pos(s_uv, s_depth);
        // Both z are negative (camera looks −z). The ray is BEHIND the surface once its z is more
        // negative than the scene's; a hit is that crossing, but only within `thickness` so the ray
        // does not tunnel through a thin object and "hit" a wall far behind it.
        const float delta = scene_view.z - s_view.z;
        if (delta > 0.0 && delta < thickness) {
            refl = texture(scene_color, s_uv).rgb;
            // Fade toward the screen border so a reflection does not pop off hard at the edge (the
            // classic SSR tell). 10% border on each side.
            const vec2 f = smoothstep(vec2(0.0), vec2(0.1), s_uv) *
                           (1.0 - smoothstep(vec2(0.9), vec2(1.0), s_uv));
            edge = f.x * f.y;
            hit = true;
        }
    }

    // Fresnel-Schlick at a dielectric F0 — reflections rise at grazing angles, the effect that sells
    // a wet floor. ndotv uses the surface→camera direction (−v_dir).
    const float ndotv = max(dot(n_view, -v_dir), 0.0);
    const float fres = 0.04 + 0.96 * pow(1.0 - ndotv, 5.0);
    const float weight = fres * rough_fade * edge;

    out_hdr = vec4(base + refl * weight, 1.0);
}
