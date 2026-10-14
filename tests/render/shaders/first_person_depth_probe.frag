// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#version 450
layout(set = 0, binding = 0) uniform sampler2D combined_depth;
layout(location = 0) out vec4 out_color;
void main() {
    float d = texelFetch(combined_depth, ivec2(gl_FragCoord.xy), 0).r;
    out_color = vec4(d, d, d, 1.0);
}
