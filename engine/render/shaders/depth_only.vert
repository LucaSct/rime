// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The depth pre-pass vertex shader (M5.6): transform position, write nothing else. The pipeline
// this feeds has NO fragment shader and NO color attachment — rasterization runs only for the
// fixed-function depth test/write, laying down the nearest-surface depth of the whole scene so
// the forward pass that follows shades each pixel exactly once (fragments behind the pre-pass
// depth fail the Equal test before their expensive BRDF runs).
//
// The uniform blocks are TRUNCATED views of the buffers the scene renderer binds: std140 layout
// guarantees `view_proj` / `model` sit at offset 0 of their blocks, so declaring only the leading
// members we use is well-defined against the same buffers the forward shaders see in full (a
// block may be smaller than the buffer bound to it). One buffer, two block shapes, zero drift on
// the fields that matter. Must match GpuFrameUniforms / GpuDrawUniforms in rime/render/passes.hpp.
//
// `invariant gl_Position` is the load-bearing line: the forward vertex shader computes the SAME
// expression, and the two pipelines must produce bit-identical positions or the forward pass's
// CompareOp::Equal depth test would sporadically reject its own geometry (the classic pre-pass
// z-fighting bug). The qualifier forbids the compiler from optimizing the two computations
// differently; docs/math/pbr.md ("the depth pre-pass contract") walks through why.
#version 450

layout(location = 0) in vec3 in_position;

layout(std140, set = 0, binding = 0) uniform FrameUniforms {
    mat4 view_proj;
} frame;

layout(std140, set = 0, binding = 1) uniform DrawUniforms {
    mat4 model;
} draw;

invariant gl_Position;

void main() {
    gl_Position = frame.view_proj * (draw.model * vec4(in_position, 1.0));
}
