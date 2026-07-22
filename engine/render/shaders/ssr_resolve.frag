// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Screen-space reflections — the resolve pass (m10.7b + m10.7c, ADR-0032 §5). One fragment per
// pixel: reconstruct the surface from the depth buffer and the m10.7a G-buffer, reflect the view ray
// about the surface normal, and MARCH that ray through the depth buffer in view space. Where the ray
// crosses behind a depth sample within a thickness tolerance it has hit on-screen geometry — sample
// the frame's own colour there.
//
// m10.7c fills the two blind spots m10.7b left named. Where the ray leaves the screen, hits the
// background, or belongs to a surface too rough for a sharp sample, the reflection now falls back to
// the DDGI PROBE FIELD sampled in the reflection direction (reflections everywhere, coupled to the GI
// thesis) instead of a flat sky constant. And the roughness fade becomes a CONE: the reflection
// blends from the sharp screen hit toward that (inherently low-frequency, pre-integrated) probe field
// as roughness rises, so a rough surface shows a BLURRED reflection rather than nothing. With DDGI
// off the probe fallback reduces to the flat ambient constant it replaced, so the pass is
// byte-for-byte the m10.7b behaviour. The reflection is added to the pixel's lit colour weighted by
// Fresnel and written to a second HDR target the tonemap then reads. docs/math/ssr.md §5/§6 derive
// the fallback, the specular-from-irradiance approximation, and the cone.
//
// Why a FULLSCREEN RASTER pass and not a compute dispatch: v1's fixed linear march has nothing a
// compute shader gives it — no groupshared cooperation, no scatter — so a fullscreen fragment is the
// simpler realization, and it writes its result as an ordinary colour attachment the tonemap then
// samples (the exact forward→tonemap path every frame already exercises).
//
// Still deferred with named triggers (docs/math/ssr.md §7): a hi-Z acceleration march (m12.0 — pure
// performance, unmeasurable on lavapipe's exact CPU depth, and the linear march is already correct)
// and a mip-chained SCREEN-colour blur for the mid-roughness band (needs an RHI mip-generation/LOD
// surface that does not exist yet; the probe cone covers rough reflections without it). Both slot in
// behind this same SsrInputs/GpuSsrUniforms seam.

#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_color; // the lit HDR frame (reflection source)
layout(set = 0, binding = 1) uniform sampler2D gbuffer;     // RG oct world normal, B roughness, A mask
layout(set = 0, binding = 2) uniform sampler2D scene_depth; // D32, Vulkan NDC z in [0,1]

layout(std140, set = 0, binding = 3) uniform SsrParams {
    mat4 proj;             // clip-from-view — projects a view-space march step to the screen
    mat4 inv_proj;         // view-from-clip — reconstructs a view position from a uv + depth
    mat4 view;             // world→view, to rotate the G-buffer's world normal into view space
    mat4 inv_view;         // view→world, to sample the probe field where the DDGI lattice lives
    vec4 extent_near_far;  // xy = render size (px), z = near, w = far
    vec4 params;           // x = max_steps, y = thickness (view units), z = unused, w = max_distance
    vec4 ambient;          // rgb = the flat sky/ambient a missed ray reflects when DDGI is off
} ssr;

// ── DDGI probe fallback bindings (m10.7c) ────────────────────────────────────────────────────────
// The octahedral irradiance/visibility atlases DdgiProbes (lighting/ddgi.hpp) maintains — the SAME
// pair the forward shader samples at binding 14/15/16, here at 4/5/6 — plus the sample-params block
// (its `enabled` flag is what turns the whole fallback on or off). When DDGI is off these are
// empty_binding's 1x1 dummies and enabled == 0, so the flat-ambient path below runs and the frame
// matches m10.7b. The four DDGI functions further down are a verbatim COPY of the forward shader's
// (no shader-include mechanism exists — the sdf_compose.comp/ddgi_trace.comp constraint); their
// derivation lives in that shader and docs/math/ddgi.md §12, cited not repeated.
layout(set = 0, binding = 4) uniform sampler2D ddgi_irradiance_atlas;
layout(set = 0, binding = 5) uniform sampler2D ddgi_visibility_atlas;

layout(std140, set = 0, binding = 6) uniform DdgiSampleParams {
    vec4 grid_origin_spacing; // xyz snapped lattice origin, w spacing
    uvec4 grid_dims_perrow;   // xyz probe counts, w atlas probes-per-row
    uvec4 enabled_pad;        // x enabled (0/1), yzw unused
} ddgi;

layout(location = 0) out vec4 out_hdr; // scene_color + reflection (a second HDR target)

// Inverse of ddgi_oct_encode (pbr_forward_shadowed.frag) — the G-buffer stores the world normal this
// way; SSR needs it both to rotate into view space (the march) and directly in world space (the
// probe fallback), so decode returns the WORLD normal.
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

// ── DDGI sampling (m10.7c; verbatim from pbr_forward_shadowed.frag, docs/math/ddgi.md §12) ────────
// The tile shape (lighting/ddgi.hpp's kDdgi*TileInterior + kDdgiTileBorder) as plain constants.
const float kDdgiIrradianceInterior = 6.0;
const float kDdgiVisibilityInterior = 14.0;

// Octahedral encode — mirrors ddgi_oct_encode (lighting/ddgi.cpp) EXACTLY: the atlas stores a
// probe's field this way, so a lookup must encode a direction the identical way to read it back.
vec2 ddgi_oct_encode(vec3 dir) {
    float denom = abs(dir.x) + abs(dir.y) + abs(dir.z);
    vec2 p = dir.xy / max(denom, 1.0e-8);
    if (dir.z <= 0.0) {
        vec2 s = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
        p = vec2((1.0 - abs(p.y)) * s.x, (1.0 - abs(p.x)) * s.y);
    }
    return p;
}

// Sample atlas `tex` at probe `global_index`'s texel for direction `dir` (the inverse of the blend
// passes' per-texel formula; see the forward shader for the coordinate algebra).
vec4 ddgi_sample_tile(sampler2D tex, uint global_index, vec3 dir, float interior) {
    uint probes_per_row = max(ddgi.grid_dims_perrow.w, 1u);
    vec2 physical = vec2(interior + 2.0); // + the 1-texel border on each side
    vec2 tile_origin =
        vec2(float(global_index % probes_per_row), float(global_index / probes_per_row)) * physical;
    vec2 oct = ddgi_oct_encode(dir);
    vec2 texel_in_tile = (oct + 1.0) * 0.5 * interior + 0.5;
    vec2 atlas_texel = tile_origin + texel_in_tile;
    vec2 atlas_size = vec2(textureSize(tex, 0));
    return texture(tex, (atlas_texel + 0.5) / atlas_size);
}

// The Chebyshev one-sided variance test (Majercik et al. 2019) — stops a probe on the wrong side of
// a wall contributing to a fragment it cannot actually see.
float ddgi_chebyshev_weight(float mean, float mean2, float dist) {
    if (dist <= mean) {
        return 1.0;
    }
    float variance = max(mean2 - mean * mean, 0.0);
    float diff = dist - mean;
    return variance / (variance + diff * diff);
}

// The irradiance the DDGI lattice reports at `world_pos` for direction `n`: the 8 bracketing probes
// of the containing cell, trilinearly weighted and each gated by the Chebyshev visibility test and a
// front-facing "wrap" weight, re-normalized by the weight actually applied. For the DIFFUSE term the
// forward shader passes the surface normal; SSR (m10.7c) passes the REFLECTION direction to read the
// field as an approximate specular radiance — see docs/math/ssr.md §5 for what that approximates and
// what it gets wrong. Clamped to the grid's own volume first (the CLAMP_TO_EDGE a texture sampler
// would apply, done here by hand) so a fragment exactly on the lattice's lowest grid line still
// brackets a real cell.
vec3 ddgi_sample_irradiance(vec3 world_pos, vec3 n) {
    float spacing = max(ddgi.grid_origin_spacing.w, 1.0e-4);
    ivec3 dims = ivec3(ddgi.grid_dims_perrow.xyz);
    vec3 rel = (world_pos - ddgi.grid_origin_spacing.xyz) / spacing;
    vec3 rel_clamped = clamp(rel, vec3(0.0), vec3(dims) - vec3(1.0));
    ivec3 base = ivec3(floor(rel_clamped));
    vec3 frac = rel_clamped - vec3(base);

    vec3 accum = vec3(0.0);
    float weight_sum = 0.0;
    for (int dz = 0; dz < 2; ++dz) {
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                ivec3 coord = base + ivec3(dx, dy, dz);
                if (any(lessThan(coord, ivec3(0))) || any(greaterThanEqual(coord, dims)))
                    continue; // past the lattice's own edge: no probe sits here

                float wx = dx == 1 ? frac.x : 1.0 - frac.x;
                float wy = dy == 1 ? frac.y : 1.0 - frac.y;
                float wz = dz == 1 ? frac.z : 1.0 - frac.z;
                float trilinear = wx * wy * wz;
                if (trilinear <= 1.0e-6)
                    continue;

                uint global = uint(coord.x) + uint(coord.y) * uint(dims.x) +
                              uint(coord.z) * uint(dims.x) * uint(dims.y);
                vec3 probe_pos = ddgi.grid_origin_spacing.xyz + vec3(coord) * spacing;

                vec3 to_probe = probe_pos - world_pos;
                float dist = length(to_probe);
                vec3 frag_to_probe = to_probe / max(dist, 1.0e-5);

                float wrap = clamp((dot(n, frag_to_probe) + 1.0) * 0.5, 0.0, 1.0);
                if (wrap <= 1.0e-6)
                    continue;

                vec2 vis =
                    ddgi_sample_tile(
                        ddgi_visibility_atlas, global, -frag_to_probe, kDdgiVisibilityInterior)
                        .rg;
                float visibility = ddgi_chebyshev_weight(vis.x, vis.y, dist);

                float total = trilinear * wrap * visibility;
                if (total <= 1.0e-6)
                    continue;

                vec3 irradiance =
                    ddgi_sample_tile(ddgi_irradiance_atlas, global, n, kDdgiIrradianceInterior).rgb;
                accum += irradiance * total;
                weight_sum += total;
            }
        }
    }
    return weight_sum > 1.0e-6 ? accum / weight_sum : vec3(0.0);
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
    const vec3 n_world = oct_decode(gb.rg); // the G-buffer stores the WORLD normal

    // Reconstruct the surface in view space (for the march) and world space (for the probe fallback),
    // and build the reflection ray in each.
    const vec3 p_view = view_pos(uv, depth);
    const vec3 world_pos = (ssr.inv_view * vec4(p_view, 1.0)).xyz;
    const vec3 n_view = normalize((ssr.view * vec4(n_world, 0.0)).xyz);
    const vec3 v_dir = normalize(p_view);      // camera (origin) → surface, view space
    const vec3 r_dir = reflect(v_dir, n_view); // the reflection ray, view space (the march)
    const vec3 cam_world = ssr.inv_view[3].xyz;
    const vec3 v_world = normalize(world_pos - cam_world);
    const vec3 r_world = reflect(v_world, n_world); // the reflection ray, world space (the fallback)

    // The wide-cone / fallback reflection: probe radiance in the reflection direction when DDGI is
    // live, else the flat sky (m10.7b's constant). Sampling the IRRADIANCE field (a cosine-weighted
    // hemisphere integral) as directional specular radiance is the documented approximation — it
    // over-blurs, which is exactly right for the rough end of the cone and an acceptable gap-filler
    // for a mirror miss (docs/math/ssr.md §5/§6).
    vec3 probe = ssr.ambient.rgb;
    if (ddgi.enabled_pad.x != 0u) {
        probe = ddgi_sample_irradiance(world_pos, r_world);
    }

    // The roughness cone: 0 = a sharp mirror (screen sample), 1 = a fully rough lobe (the pre-
    // integrated probe field). Between, the reflection blends from one toward the other.
    const float cone = smoothstep(0.25, 0.55, roughness);

    // March the screen for the sharp end of the cone. A fully-rough surface (cone >= 1) is pure probe
    // and skips the march entirely — a rough surface pays nothing for the screen trace.
    vec3 sharp = probe; // a miss falls back to the probe (the flat sky when DDGI is off)
    float edge = 1.0;   // a miss uses the probe fully; only a real hit fades toward the border
    if (cone < 1.0) {
        const int max_steps = int(ssr.params.x);
        const float thickness = ssr.params.y;
        const float near = ssr.extent_near_far.z;
        const float step_len = ssr.params.w / float(max_steps);
        for (int i = 1; i <= max_steps; ++i) {
            const vec3 s_view = p_view + r_dir * (step_len * float(i));
            if (s_view.z > -near)
                break; // stepped in front of the near plane — off the top of the frustum
            const vec4 clip = ssr.proj * vec4(s_view, 1.0);
            if (clip.w <= 0.0)
                break;
            const vec2 s_uv = (clip.xy / clip.w) * 0.5 + 0.5;
            if (any(lessThan(s_uv, vec2(0.0))) || any(greaterThan(s_uv, vec2(1.0))))
                break; // ran off the side of the screen — a miss, fall back to the probe
            const float s_depth = texture(scene_depth, s_uv).r;
            if (s_depth >= 1.0)
                continue; // that pixel is background (no surface to hit); keep marching
            const vec3 scene_view = view_pos(s_uv, s_depth);
            // Both z are negative (camera looks −z). The ray is BEHIND the surface once its z is more
            // negative than the scene's; a hit is that crossing, but only within `thickness` so the
            // ray does not tunnel through a thin object and "hit" a wall far behind it.
            const float delta = scene_view.z - s_view.z;
            if (delta > 0.0 && delta < thickness) {
                sharp = texture(scene_color, s_uv).rgb;
                // Fade toward the screen border so a reflection does not pop off hard at the edge
                // (the classic SSR tell). 10% border on each side; past it the probe fills in.
                const vec2 f = smoothstep(vec2(0.0), vec2(0.1), s_uv) *
                               (1.0 - smoothstep(vec2(0.9), vec2(1.0), s_uv));
                edge = f.x * f.y;
                break;
            }
        }
    }

    // Blend the sharp screen hit toward the probe at the screen border (an edge hit fades into the
    // probe field instead of popping off a hard cutoff — the m10.7b edge fade, now filling with real
    // radiance rather than fading to nothing), then blend the whole reflection toward the blurry
    // probe as roughness rises.
    const vec3 mirror = mix(probe, sharp, edge);
    const vec3 refl = mix(mirror, probe, cone);

    // Fresnel-Schlick at a dielectric F0 — reflections rise at grazing angles (the wet-floor tell).
    // The whole reflection (screen or probe) is the specular lobe, so it is modulated by Fresnel
    // exactly as an environment reflection would be. ndotv uses the surface→camera direction (−v_dir).
    const float ndotv = max(dot(n_view, -v_dir), 0.0);
    const float fres = 0.04 + 0.96 * pow(1.0 - ndotv, 5.0);

    out_hdr = vec4(base + refl * fres, 1.0);
}
