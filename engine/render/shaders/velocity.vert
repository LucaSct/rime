// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The motion-vector pass's vertex shader (ADR-0078 step 1c). It does two jobs with one set of
// inputs: position the vertex EXACTLY where the depth pre-pass did (so the fragment stage's
// CompareOp::Equal depth test keeps precisely the visible surface), and hand the fragment stage the
// clip-space position of the same point in the current and the previous frame.
//
// gl_Position uses the JITTERED matrix (frame.view_proj) with the text-identical expression of
// depth_only.vert and pbr_forward.vert, and is `invariant` for the same reason they are: the Equal
// test only works if every pipeline rasterizes each triangle at bit-identical positions.
//
// The two clip positions that become the velocity use the UNJITTERED matrices instead. Both ends
// unjittered: a velocity that carried this frame's sub-pixel jitter would make the resolve's
// neighbourhood clamp fight the very pattern it is averaging, so jitter belongs in where pixels are
// SAMPLED and never in where surfaces MOVED.
//
// The FrameUniforms block here is the FULL declaration (it must reach the members at offsets 752
// and 816); it must match GpuFrameUniforms / GpuDrawUniforms in rime/render/passes.hpp, and the
// three other full declarations (pbr_forward.vert / .frag, pbr_forward_shadowed.frag).
#version 450

layout(location = 0) in vec3 in_position;

struct DirLight {
    vec4 direction;
    vec4 radiance;
};

struct PointLight {
    vec4 position;
    vec4 radiance;
};

layout(std140, set = 0, binding = 0) uniform FrameUniforms {
    mat4 view_proj;
    vec4 camera_pos;
    vec4 ambient;
    uvec4 light_counts;
    DirLight dir_lights[4];
    PointLight point_lights[16];
    mat4 view_proj_unjittered;
    mat4 prev_view_proj;
} frame;

layout(std140, set = 0, binding = 1) uniform DrawUniforms {
    mat4 model;
    mat4 normal_matrix;
    vec4 base_color;
    vec4 params;
    vec4 emissive;
    mat4 prev_model;
} draw;

// Clip-space (NOT yet divided by w): clip space is linear in the vertex attributes, so the
// rasterizer's perspective-correct interpolation of it is exact, and the divide by w is done per
// pixel in the fragment shader. Dividing here and interpolating NDC would be wrong across a
// triangle that spans a depth range.
layout(location = 0) out vec4 v_clip_current;
layout(location = 1) out vec4 v_clip_previous;

// `precise` forbids fused multiply-add contraction and re-association in everything that feeds
// these two outputs. A static point must come out with BIT-identical current and previous clip
// positions, and two textually identical expressions are only guaranteed to round identically if
// the compiler may not fuse one differently from the other.
precise v_clip_current;
precise v_clip_previous;

invariant gl_Position;

void main() {
    v_clip_current = frame.view_proj_unjittered * (draw.model * vec4(in_position, 1.0));
    v_clip_previous = frame.prev_view_proj * (draw.prev_model * vec4(in_position, 1.0));
    gl_Position = frame.view_proj * (draw.model * vec4(in_position, 1.0));
}
