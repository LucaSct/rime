// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The virtual-geometry visibility pass, fragment stage (M18 steps 1-2). A visibility buffer is
// the pick pass's ID idea applied to every pixel: no shading here, only "which triangle won the
// depth test". The id arrives pre-packed (visibility-ID ABI version 2) with a zero triangle
// field; the triangle index from the vertex stage fills bits [6:0]. The CPU gate rejects clusters
// with more than 128 triangles, so the OR can never spill into the slot bits.
//
// The second output carries window-space depth as raw float bits. Depth attachments cannot be
// read back through the RHI's colour-aspect copy, and there is no R32Float colour format, so the
// bit pattern travels in an integer target: floatBitsToUint is exact, and 0 (the clear) is +0.0.
#version 450

layout(location = 0) flat in uint in_triangle;

layout(location = 0) out uint out_visibility;
layout(location = 1) out uint out_depth_bits;

layout(push_constant) uniform Pc {
    mat4 mvp;
    uint id;
    uint index_base;
    uint vertex_base;
    uint stride_words;
} pc;

void main() {
    out_visibility = pc.id | in_triangle;
    out_depth_bits = floatBitsToUint(gl_FragCoord.z);
}
