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
//
// Field colormap (C1): when a field is bound (vmax > vmin in the push constant), the surface albedo is
// replaced by a colormap of the simulation field sampled at the fragment's world position — the same
// shade then lights it, so the computed temperature/etc. reads on the part and on the cut-revealed
// interior. With no field bound, the path below is identical to the plain lit shade. See colormap.md.
#version 450

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;

layout(location = 0) out vec4 out_color;

// Must match MeshPush in mesh_render.hpp and the block in mesh.vert (one shared 128-byte block).
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 cam_pos;     // xyz = eye position (world)
    vec4 clip_plane;  // xyz = unit normal, w = signed offset; discard where dot(N,p) > w (disabled: N=0)
    vec4 field_scale; // xyz = world->uvw scale, w = vmin
    vec4 field_bias;  // xyz = world->uvw bias,  w = vmax  (field is "on" iff vmax > vmin)
} pc;

// The field volume: R = value (dilated into the absent shell), G = validity. 3-D texture, trilinear.
layout(set = 0, binding = 0) uniform sampler3D u_field;

// 5-stop "turbo-lite" transfer function: blue (cold) -> cyan -> green -> yellow -> red (hot). Ordered
// enough to read a field at a glance and intuitive for temperature; a full perceptual map (viridis/
// turbo) is a drop-in later. The same stops draw the legend bar. See docs/math/colormap.md.
vec3 colormap(float t) {
    const vec3 c0 = vec3(0.20, 0.30, 0.80); // 0.00 blue
    const vec3 c1 = vec3(0.10, 0.70, 0.90); // 0.25 cyan
    const vec3 c2 = vec3(0.25, 0.80, 0.30); // 0.50 green
    const vec3 c3 = vec3(0.95, 0.85, 0.20); // 0.75 yellow
    const vec3 c4 = vec3(0.90, 0.20, 0.15); // 1.00 red
    float x = clamp(t, 0.0, 1.0) * 4.0;
    if (x < 1.0) return mix(c0, c1, x);
    if (x < 2.0) return mix(c1, c2, x - 1.0);
    if (x < 3.0) return mix(c2, c3, x - 2.0);
    return mix(c3, c4, x - 3.0);
}

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

    // Albedo: a flat per-part tint in assembly mode (E1), else the field colormap when a field is bound,
    // else the neutral light-metal base. Assembly mode (cam_pos.w > 1.5) carries the part colour in
    // field_scale.xyz — the same push slots the colormap uses, free here because an assembly binds no
    // field; the two-sided lighting below then shades the tint exactly like any other albedo.
    vec3 base = vec3(0.80, 0.80, 0.82);
    bool assembly = pc.cam_pos.w > 1.5;
    bool field_on = pc.field_bias.w > pc.field_scale.w; // vmax > vmin
    if (assembly) {
        base = pc.field_scale.xyz;
    } else if (field_on) {
        vec3 uvw = v_world_pos * pc.field_scale.xyz + pc.field_bias.xyz;
        float value = texture(u_field, uvw).r;
        float t = (value - pc.field_scale.w) / (pc.field_bias.w - pc.field_scale.w);
        base = colormap(t);
    }
    vec3 color = base * (0.5 * ambient + 0.9 * diffuse);

    // Fresnel rim: brightens grazing angles to outline the form.
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    color += vec3(0.25) * rim;

    out_color = vec4(color, 1.0);
}
