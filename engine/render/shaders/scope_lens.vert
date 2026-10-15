// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#version 450
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(std140, set = 0, binding = 0) uniform LensUniforms {
    mat4 view_proj;
    mat4 model;
    mat4 normal_matrix;
    mat4 optic_from_world;
    mat4 world_from_optic;
    mat4 optical_view_proj;
    vec4 main_eye;
    vec4 aperture; // radius, relief, relief tolerance, eye box radius
    vec4 optical; // focus distance, tan(half FOV), intensity, reflection strength
    vec4 glass; // roughness, reticle strength
} lens;
layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_world_normal;
invariant gl_Position;
void main() {
    v_world_pos = (lens.model * vec4(in_position, 1.0)).xyz;
    v_world_normal = mat3(lens.normal_matrix) * in_normal;
    // Matches the first-person prepass expression. No screen-sized mask or overlay geometry.
    gl_Position = lens.view_proj * (lens.model * vec4(in_position, 1.0));
}
