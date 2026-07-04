// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the multiple-render-targets proof: one invocation writes two color
// attachments at once — layout(location = i) lands in RenderingInfo::colors[i]. Distinct constants
// per target so the test can tell exactly which output reached which image.
#version 450

layout(location = 0) out vec4 out_a;
layout(location = 1) out vec4 out_b;

void main() {
    out_a = vec4(1.0, 0.5, 0.0, 1.0); // orange → attachment 0
    out_b = vec4(0.0, 0.5, 1.0, 1.0); // cyan   → attachment 1
}
