// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Solid green. Used by both passes of the stencil proof: the marking pass writes no colour (the
// pipeline masks it off, updating only stencil), the fill pass writes green where the stencil test passes.
#version 450

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(0.0, 1.0, 0.0, 1.0);
}
