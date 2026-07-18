// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The ID-buffer pick pass, fragment stage (m9.6): every fragment of a draw writes that draw's id
// into the R32Uint target. No shading, no texturing — the depth test alone decides which draw's id
// survives at the pixel, which is exactly "the nearest visible surface wins" a click expects.
// The target is cleared to 0 ("nothing"); ScenePicker therefore numbers draws from 1.
#version 450

layout(location = 0) out uint out_id;

layout(push_constant) uniform Pc {
    mat4 mvp;
    uint id;
} pc;

void main() {
    out_id = pc.id;
}
