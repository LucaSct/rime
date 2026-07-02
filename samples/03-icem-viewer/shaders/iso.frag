// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// GPU raymarched isosurface / direct volume rendering of a scalar field (C2). For each pixel it casts a
// world-space ray (reconstructed from the inverse view-projection), transforms it into the field's
// texture (uvw) space, clips it to the volume box [0,1]^3, and marches:
//   * isosurface: find the first crossing of value = isovalue, shade it with the field gradient as the
//     surface normal and the colormap of the isovalue → a lit isosurface (an isotherm, say);
//   * DVR (direct volume rendering): accumulate colour·opacity front-to-back for a cloudy view.
// The volume is the same RGBA32F field texture C1 builds (R = value, G = validity); cells outside the
// solid (G < 0.5) are skipped. See docs/math/raymarch.md.
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler3D u_field; // R = value, G = validity

layout(push_constant) uniform Push {
    mat4 inv_vp;      // world-from-clip (inverse view-projection), for per-pixel rays
    vec4 field_scale; // xyz = world->uvw scale, w = isovalue
    vec4 field_bias;  // xyz = world->uvw bias,  w = vmin
    vec4 meta;        // x = vmax, y = step count, z = mode (0 isosurface, 1 DVR)
} pc;

vec3 colormap(float t) {
    const vec3 c0 = vec3(0.20, 0.30, 0.80);
    const vec3 c1 = vec3(0.10, 0.70, 0.90);
    const vec3 c2 = vec3(0.25, 0.80, 0.30);
    const vec3 c3 = vec3(0.95, 0.85, 0.20);
    const vec3 c4 = vec3(0.90, 0.20, 0.15);
    float x = clamp(t, 0.0, 1.0) * 4.0;
    if (x < 1.0) return mix(c0, c1, x);
    if (x < 2.0) return mix(c1, c2, x - 1.0);
    if (x < 3.0) return mix(c2, c3, x - 2.0);
    return mix(c3, c4, x - 3.0);
}

float value_at(vec3 uvw) { return texture(u_field, uvw).r; }
float valid_at(vec3 uvw) { return texture(u_field, uvw).g; }

void main() {
    // World ray from the inverse view-projection: near and far points at this pixel's NDC.
    vec2 ndc = v_uv * 2.0 - 1.0;
    vec4 pn = pc.inv_vp * vec4(ndc, 0.0, 1.0);
    vec4 pf = pc.inv_vp * vec4(ndc, 1.0, 1.0);
    vec3 ro = pn.xyz / pn.w;
    vec3 rd = normalize(pf.xyz / pf.w - ro);

    // Into texture (uvw) space: the affine is a diagonal scale + bias, so directions just scale.
    vec3 o = ro * pc.field_scale.xyz + pc.field_bias.xyz;
    vec3 d = rd * pc.field_scale.xyz;

    // Ray vs the unit box [0,1]^3 (slab method).
    vec3 invd = 1.0 / d;
    vec3 ta = (vec3(0.0) - o) * invd;
    vec3 tb = (vec3(1.0) - o) * invd;
    vec3 tlo = min(ta, tb), thi = max(ta, tb);
    float tmin = max(max(tlo.x, tlo.y), tlo.z);
    float tmax = min(min(thi.x, thi.y), thi.z);
    if (tmax <= max(tmin, 0.0)) discard; // ray misses the volume
    tmin = max(tmin, 0.0);

    const int N = int(pc.meta.y);
    const float dt = (tmax - tmin) / float(N);
    const float iso = pc.field_scale.w;
    const float vmin = pc.field_bias.w;
    const float vmax = pc.meta.x;
    const float inv_range = 1.0 / max(vmax - vmin, 1e-12);

    if (pc.meta.z > 0.5) {
        // Direct volume rendering: front-to-back compositing of the colormap, opacity rising with value.
        vec4 acc = vec4(0.0);
        for (int i = 0; i < N; ++i) {
            vec3 p = o + d * (tmin + (float(i) + 0.5) * dt);
            if (valid_at(p) < 0.5) continue;
            float tt = clamp((value_at(p) - vmin) * inv_range, 0.0, 1.0);
            float a = 0.07 * tt; // emphasise the hot end
            acc.rgb += (1.0 - acc.a) * a * colormap(tt);
            acc.a += (1.0 - acc.a) * a;
            if (acc.a > 0.98) break;
        }
        if (acc.a < 0.01) discard;
        out_color = vec4(acc.rgb, 1.0);
        return;
    }

    // Isosurface: first sign change of (value - isovalue) inside the solid.
    float prev = value_at(o + d * tmin) - iso;
    for (int i = 1; i <= N; ++i) {
        float t = tmin + float(i) * dt;
        vec3 p = o + d * t;
        if (valid_at(p) < 0.5) {
            prev = value_at(p) - iso;
            continue;
        }
        float cur = value_at(p) - iso;
        if (prev * cur <= 0.0) {
            float w = prev / (prev - cur + 1e-12); // linear refine of the crossing
            vec3 hit = o + d * (t - dt + w * dt);
            const float e = 1.0 / 256.0; // gradient step in uvw (within a cell)
            vec3 g = vec3(value_at(hit + vec3(e, 0, 0)) - value_at(hit - vec3(e, 0, 0)),
                          value_at(hit + vec3(0, e, 0)) - value_at(hit - vec3(0, e, 0)),
                          value_at(hit + vec3(0, 0, e)) - value_at(hit - vec3(0, 0, e)));
            vec3 nrm = normalize(g + vec3(1e-6));
            const vec3 L = normalize(vec3(0.4, 0.85, 0.5));
            float diff = abs(dot(nrm, L)); // two-sided (the iso normal's sign is arbitrary)
            vec3 base = colormap(clamp((iso - vmin) * inv_range, 0.0, 1.0));
            out_color = vec4(base * (0.35 + 0.75 * diff), 1.0);
            return;
        }
        prev = cur;
    }
    discard; // the isovalue is not crossed along this ray
}
