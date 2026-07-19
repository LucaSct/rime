// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The editor-gizmo overlay, fragment stage (m9.6b): every fragment writes the draw's flat colour.
// No lighting, no texturing — a gizmo is UI in the 3D view, and its job is to be legible, not lit.
// The same shader serves the opaque handle lines (blend off) and the selection tint (the tint
// pipeline turns on classic alpha blending, so color.a is the tint strength over the shaded mesh).
#version 450

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Pc {
    mat4 mvp;
    vec4 color;
} pc;

void main() {
    out_color = pc.color;
}
