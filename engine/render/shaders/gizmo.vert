// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The editor-gizmo overlay, vertex stage (m9.6b): transform position, nothing else — the pick
// pass's minimal-vertex idea, now drawing INTO the frame instead of asking a question of it. One
// push-constant block per draw (MVP + a flat RGBA colour) serves both users of this shader pair:
//   * the gizmo handle geometry (line lists: axis shafts, arrowheads, rings, cube ends), where
//     each axis is one draw so it can carry its own colour (red/green/blue, or the highlight);
//   * the selection tint, re-drawing the selected mesh with a translucent colour (the pipeline
//     supplies alpha blending; the shader is identical).
// A gizmo renders a handful of draws over an editor scene, so push constants — the cheapest
// per-draw state there is — are the whole binding model. Must match GizmoRenderer's GizmoPush
// (gizmo_renderer.cpp).
#version 450

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform Pc {
    mat4 mvp;   // clip-from-object: view_proj * model, folded on the CPU per draw
    vec4 color; // consumed by the fragment stage (declared identically there)
} pc;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
}
