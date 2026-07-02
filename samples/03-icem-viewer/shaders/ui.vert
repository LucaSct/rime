// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vertex shader for the from-scratch UI overlay (E2). UI geometry is authored in **screen pixels** (top-
// left origin, y down); this maps it straight to Vulkan clip space, whose NDC also has y pointing down,
// so x=0 → left, y=0 → top. Depth 0 (the overlay draws on top, depth test off). Forwards the atlas
// texcoord and the per-vertex colour. See docs/math/ui-text-layout.md.
#version 450

layout(location = 0) in vec2 in_pos;   // screen pixels
layout(location = 1) in vec2 in_uv;    // atlas texcoord (u < 0 ⇒ solid, no texture)
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(push_constant) uniform Push {
    vec2 screen; // framebuffer size in pixels
    vec2 pad;
} pc;

void main() {
    vec2 ndc = vec2(in_pos.x / pc.screen.x * 2.0 - 1.0, in_pos.y / pc.screen.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
