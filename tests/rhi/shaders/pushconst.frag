// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the push-constant proof: emit the color handed in via a push constant. The whole
// point is to show that a small block of per-draw data reaches the shader and can change between draws
// with no descriptor set or buffer.
#version 450

layout(push_constant) uniform Push {
    vec4 color;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = pc.color;
}
