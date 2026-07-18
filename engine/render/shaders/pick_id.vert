// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The ID-buffer pick pass, vertex stage (m9.6): transform position, nothing else — the depth-only
// idea again, but for a different question. The whole per-draw state is one push-constant block
// (MVP + the draw's id): a pick renders exactly once per click at a 1x1 target, so there is no
// shared frame to amortize a uniform buffer over — push constants are the cheapest way to hand a
// single draw its 68 bytes. Must match ScenePicker's DrawPush (scene_picker.cpp).
#version 450

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform Pc {
    mat4 mvp;   // clip-from-object: view_proj * model, folded on the CPU per draw
    uint id;    // consumed by the fragment stage (declared identically there)
} pc;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
}
