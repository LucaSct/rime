// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the depth-test proof: emit the interpolated vertex color, opaque. All the
// interesting behavior is the fixed-function depth test deciding *whether* this fragment is kept.
#version 450

layout(location = 0) in vec3 frag_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(frag_color, 1.0);
}
