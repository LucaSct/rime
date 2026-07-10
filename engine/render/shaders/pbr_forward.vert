// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Forward-PBR vertex shader (M5.6, extended M6.4): move a registry vertex into clip space and hand
// the fragment shader what shading needs — the world-space position (for light and view directions),
// the world-space normal, the uv, and the world-space tangent + handedness for normal mapping's TBN
// frame. Uniform blocks must match GpuFrameUniforms / GpuDrawUniforms in rime/render/passes.hpp (the
// C++ side static_asserts the std140 offsets).
//
// Normals transform by the inverse-transpose of the model matrix, not the model matrix itself:
// under non-uniform scale the direct transform would tilt normals off the surface (squash a
// sphere and its normals must splay outward, the opposite of what squashing position vectors
// does). The CPU computes that matrix once per draw; mat3() takes its rotation/scale part.
//
// gl_Position is computed with the TEXT-IDENTICAL expression the depth pre-pass uses, and both
// are marked `invariant`: the forward pipeline depth-tests with CompareOp::Equal against the
// pre-pass's depth, which only works if both pipelines rasterize every triangle at bit-identical
// positions. See depth_only.vert for the full story.
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent; // xyz = tangent (object space), w = handedness sign (M6.4)

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
    vec4 camera_pos;   // xyz = eye position (world), w unused
    vec4 ambient;      // rgb = constant ambient radiance (linear), w unused
    uvec4 light_counts; // x = directional lights, y = point lights, zw unused
    DirLight dir_lights[4];
    PointLight point_lights[16];
} frame;

layout(std140, set = 0, binding = 1) uniform DrawUniforms {
    mat4 model;
    mat4 normal_matrix; // inverse-transpose of model, for normals (upper 3x3 is what counts)
    vec4 base_color;    // linear-space factor
    vec4 params;        // x = metallic, y = roughness, z = normal_scale, w = occlusion_strength
    vec4 emissive;      // rgb = emissive factor (linear); used in the fragment shader
} draw;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_world_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_world_tangent; // for the fragment's TBN (M6.4)
layout(location = 4) out float v_tangent_w;    // handedness, carried through unchanged

invariant gl_Position;

void main() {
    vec4 world = draw.model * vec4(in_position, 1.0);
    v_world_pos = world.xyz;
    v_world_normal = mat3(draw.normal_matrix) * in_normal;
    // The tangent is a surface DIRECTION (a difference of positions), so it rides the model matrix
    // itself — not the inverse-transpose the normal needs. The handedness sign is invariant under
    // the transform, so it passes straight through. See docs/math/tangent-space.md §6.
    v_world_tangent = mat3(draw.model) * in_tangent.xyz;
    v_tangent_w = in_tangent.w;
    v_uv = in_uv;
    gl_Position = frame.view_proj * (draw.model * vec4(in_position, 1.0));
}
