// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Forward-PBR fragment shader (M5.6): one pixel of the rendering equation, solved for punctual
// lights. The BRDF is the industry-standard Cook-Torrance microfacet model —
//
//     f = kd · albedo/π  +  D(h) · V(v,l) · F(v,h)
//
// with GGX as the normal distribution D, height-correlated Smith as the visibility V (the
// geometry term with the 1/(4·NoV·NoL) folded in), and Schlick's approximation for the Fresnel
// term F. Diffuse is Lambert, scaled by the energy the specular lobe didn't take (kd) so the
// surface never reflects more light than it receives. Every term, constant, and the reason this
// sum has exactly this shape is derived step by step in docs/math/pbr.md — this file cites, the
// doc explains.
//
// Everything here happens in LINEAR space and the result is RADIANCE, not a display color: values
// above 1 are normal (that is what the RGBA16F target is for) and the tonemap pass turns them
// into something a monitor can show. ADR-0022 records the pipeline-level choices.
#version 450

struct DirLight {
    vec4 direction; // xyz = the direction the light TRAVELS (world), w unused
    vec4 radiance;  // rgb = color * intensity (linear), w unused
};

struct PointLight {
    vec4 position; // xyz = world position, w = falloff radius
    vec4 radiance; // rgb = color * intensity (linear), w unused
};

layout(std140, set = 0, binding = 0) uniform FrameUniforms {
    mat4 view_proj;
    vec4 camera_pos;
    vec4 ambient;
    uvec4 light_counts;
    DirLight dir_lights[4];
    PointLight point_lights[16];
} frame;

layout(std140, set = 0, binding = 1) uniform DrawUniforms {
    mat4 model;
    mat4 normal_matrix;
    vec4 base_color;
    vec4 params;   // x = metallic, y = roughness, z = normal_scale, w = occlusion_strength
    vec4 emissive; // rgb = emissive factor (linear)
} draw;

// The five material maps (M6.4), each multiplied with / driving its factor. Every material binds all
// five — an untextured slot gets a 1x1 fallback (white, or flat-normal for the normal slot) — so
// these fetches never branch and one pipeline serves every permutation. base-color and emissive are
// sRGB-format (the sampler decodes to linear); normal / metallic-roughness / occlusion are linear
// data sampled verbatim.
layout(set = 0, binding = 2) uniform sampler2D base_color_tex;
layout(set = 0, binding = 3) uniform sampler2D metallic_roughness_tex; // G = roughness, B = metallic
layout(set = 0, binding = 4) uniform sampler2D normal_tex;             // tangent-space normal
layout(set = 0, binding = 5) uniform sampler2D occlusion_tex;          // R = ambient occlusion
layout(set = 0, binding = 6) uniform sampler2D emissive_tex;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_world_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_world_tangent; // interpolated tangent (M6.4)
layout(location = 4) in float v_tangent_w;    // handedness sign for the bitangent

layout(location = 0) out vec4 out_hdr;

const float kPi = 3.14159265358979;

// D — GGX / Trowbridge-Reitz normal distribution: the statistical concentration of microfacet
// normals around n. α = roughness² (Disney's perceptual remap: even slider steps look even).
// GGX's fat tail is why its highlights have the soft halo real materials show.
float d_ggx(float n_dot_h, float alpha) {
    float a2 = alpha * alpha;
    float t = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * t * t);
}

// V — height-correlated Smith visibility: what fraction of microfacets both the light and the
// eye actually see (masking + shadowing), divided by the 4·NoV·NoL projection Jacobian of the
// half-vector parameterization. Folding the two together (Heitz 2014) is numerically kinder than
// computing G alone and dividing — no 0/0 at grazing angles.
float v_smith_ggx(float n_dot_v, float n_dot_l, float alpha) {
    float a2 = alpha * alpha;
    float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// F — Fresnel-Schlick: reflectance grows from f0 (head-on) to 1 (grazing) as the fifth power of
// the complement — the cheap curve that fits the full Fresnel equations to within a percent. f0
// is 0.04 for dielectrics (glass/plastic/wood all sit near 4%) and the base color for metals.
vec3 f_schlick(float v_dot_h, vec3 f0) {
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

// One punctual light's contribution: BRDF × incident radiance × the geometry cosine. `l` points
// from the surface TOWARD the light; `radiance` is what arrives at this point (falloff already
// applied for point lights).
vec3 shade_light(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, float metallic, float alpha) {
    float n_dot_l = dot(n, l);
    if (n_dot_l <= 0.0)
        return vec3(0.0); // the light is behind the surface — no transport, and no negative light
    vec3 h = normalize(v + l);
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(v, h), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = f_schlick(v_dot_h, f0);
    vec3 specular = d_ggx(n_dot_h, alpha) * v_smith_ggx(n_dot_v, n_dot_l, alpha) * fresnel;

    // Energy split: what Fresnel reflected specularly cannot ALSO scatter diffusely, and metals
    // have no diffuse at all (their "color" is the F0 of the specular lobe).
    vec3 kd = (vec3(1.0) - fresnel) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / kPi;

    return (diffuse + specular) * radiance * n_dot_l;
}

// Reconstruct the world-space TBN from the interpolated tangent basis and rotate the tangent-space
// normal out of the map. Interpolation leaves the tangent slightly off-perpendicular to the normal,
// so re-orthonormalize with one Gram-Schmidt step; the bitangent is rebuilt from the handedness sign
// (w·cross(N,T), the glTF convention — correct even under mirrored UVs). normal_scale (params.z)
// flattens the tangent-plane XY. The flat-normal fallback decodes to (0,0,1), leaving N unchanged, so
// an un-mapped surface costs only this arithmetic. Derivation: docs/math/tangent-space.md §6.
vec3 perturb_normal(vec3 world_normal) {
    vec3 N = normalize(world_normal);
    vec3 T = normalize(v_world_tangent - N * dot(N, v_world_tangent));
    vec3 B = v_tangent_w * cross(N, T);
    vec3 c = texture(normal_tex, v_uv).xyz;
    vec3 n_tangent = vec3((2.0 * c.xy - 1.0) * draw.params.z, 2.0 * c.z - 1.0);
    return normalize(mat3(T, B, N) * n_tangent); // columns T,B,N: TBN·n = nx·T + ny·B + nz·N
}

void main() {
    vec3 n = perturb_normal(v_world_normal);
    vec3 v = normalize(frame.camera_pos.xyz - v_world_pos);
    vec3 albedo = draw.base_color.rgb * texture(base_color_tex, v_uv).rgb;

    // Metallic-roughness map: glTF packs roughness in G and metallic in B; each multiplies its
    // factor (the white fallback → the factor alone). Roughness → α is floored as in M5.6: α = 0
    // makes D a delta lobe — a single infinitely bright sample under a punctual light.
    vec2 mr = texture(metallic_roughness_tex, v_uv).gb;
    float metallic = draw.params.x * mr.y;
    float roughness = clamp(draw.params.y * mr.x, 0.045, 1.0);
    float alpha = roughness * roughness;

    // Ambient occlusion scales the AMBIENT term ONLY — never the direct lights, which cast their own
    // real shadows; multiplying direct light by a baked AO map double-counts and greys out lit
    // contact edges (docs/math/pbr.md). occlusion_strength (params.w) lerps the map toward 1.
    float ao = mix(1.0, texture(occlusion_tex, v_uv).r, draw.params.w);

    // Constant ambient: a deliberately crude stand-in for global illumination (M10) — treat
    // `ambient` as isotropic irradiance and reflect it diffusely, occluded by AO. Enough to keep
    // unlit sides from going pitch black, honest about being a hack (docs/math/pbr.md §ambient).
    vec3 out_radiance = albedo * frame.ambient.rgb * ao;

    for (uint i = 0u; i < frame.light_counts.x && i < 4u; ++i) {
        vec3 l = normalize(-frame.dir_lights[i].direction.xyz); // travel direction → toward-light
        out_radiance +=
            shade_light(n, v, l, frame.dir_lights[i].radiance.rgb, albedo, metallic, alpha);
    }

    for (uint i = 0u; i < frame.light_counts.y && i < 16u; ++i) {
        vec3 to_light = frame.point_lights[i].position.xyz - v_world_pos;
        float dist2 = max(dot(to_light, to_light), 1e-4);
        float dist = sqrt(dist2);
        vec3 l = to_light / dist;
        // Physically-based falloff: inverse-square (energy over a growing sphere), times a
        // smooth window that takes it to exactly zero at the light's radius so a light's reach
        // is boundable (the many-lights culling seam). Window: (1 − (d/r)⁴)², saturated — the
        // Frostbite/Karis shape; derivation in docs/math/pbr.md §falloff.
        float r = max(frame.point_lights[i].position.w, 1e-3);
        float q = dist / r;
        float q4 = q * q * q * q;
        float window = clamp(1.0 - q4, 0.0, 1.0);
        float falloff = window * window / dist2;
        vec3 radiance = frame.point_lights[i].radiance.rgb * falloff;
        out_radiance += shade_light(n, v, l, radiance, albedo, metallic, alpha);
    }

    // Emissive: the surface's own light, added AFTER the BRDF so it shows even with no light present
    // (glowing screens, lava). Factor × map; the default-black factor with the white fallback is 0,
    // so a non-emissive material pays nothing.
    out_radiance += draw.emissive.rgb * texture(emissive_tex, v_uv).rgb;

    out_hdr = vec4(out_radiance, 1.0);
}
