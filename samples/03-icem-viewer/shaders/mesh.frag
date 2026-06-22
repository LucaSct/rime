// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the ICEM viewer's lit mesh pass. A deliberately simple, robust "studio" shade so
// a computed part reads clearly before the real PBR/render-graph lighting (M5) exists:
//   * a single key light (Lambert / N·L diffuse),
//   * hemispherical ambient (a cool "sky" from above fading to a warm "ground" below) as soft fill,
//   * a Fresnel rim term to pop the silhouette.
// Lighting is two-sided (the normal is flipped to face the viewer) so a cross-section's interior walls
// — which face inward — are lit too, not left black. Constants are baked in for now; they become
// material/lighting inputs when the render graph lands.
#version 450

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 cam_pos;
    vec4 clip_plane; // xyz = unit normal, w = signed offset; discard where dot(N,p) > w (disabled: N=0)
} pc;

void main() {
    // Cross-section: cut away the half-space in front of the plane so the interior is revealed.
    if (dot(pc.clip_plane.xyz, v_world_pos) > pc.clip_plane.w) discard;

    vec3 N = normalize(v_normal);
    vec3 V = normalize(pc.cam_pos.xyz - v_world_pos);
    // A face that the cut exposes points roughly along the section normal and away from the viewer;
    // flipping the normal toward the viewer (two-sided) lights those interior walls instead of leaving
    // them black, which is what makes the section read as a solid cutaway.
    if (dot(N, V) < 0.0) N = -N;

    const vec3 L = normalize(vec3(0.4, 0.85, 0.5)); // fixed key light direction (world space)
    float diffuse = max(dot(N, L), 0.0);

    // Hemispherical ambient: blend ground→sky by how "up" the normal points.
    float up = 0.5 * N.y + 0.5;
    const vec3 sky = vec3(0.55, 0.60, 0.70);
    const vec3 ground = vec3(0.20, 0.18, 0.16);
    vec3 ambient = mix(ground, sky, up);

    const vec3 base = vec3(0.80, 0.80, 0.82); // light-metal albedo
    vec3 color = base * (0.5 * ambient + 0.9 * diffuse);

    // Fresnel rim: brightens grazing angles to outline the form.
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    color += vec3(0.25) * rim;

    out_color = vec4(color, 1.0);
}
