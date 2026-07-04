// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the compute→graphics proof: sample the texture at this fragment's own
// texel (gl_FragCoord is pixel-center, so dividing by the texture size hits texel centers — with
// a Nearest sampler the fetch is exact). Whatever the compute pass stored at (x, y) must appear
// at framebuffer pixel (x, y).
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(tex, gl_FragCoord.xy / vec2(textureSize(tex, 0)));
}
