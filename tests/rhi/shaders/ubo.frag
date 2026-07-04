// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the uniform-buffer proof: emit the color read from a uniform block at
// set 0 / binding 0 — the ADR-0020 descriptor path (declared binding layout → transient set per
// draw), where pushconst.frag proves the descriptor-less push-constant path.
#version 450

layout(set = 0, binding = 0) uniform Ubo {
    vec4 color;
} u;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = u.color;
}
