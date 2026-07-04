// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the mipmap proof: sample the texture at an EXPLICIT mip level (textureLod),
// pushed per draw. Explicit LOD removes the driver's derivative-based level selection from the
// test — what's asserted is purely "what does level N of the generated chain hold", which is
// deterministic. The 4.0 divisor matches the test's 4×4 render target.
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Push {
    float lod;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = textureLod(tex, gl_FragCoord.xy / 4.0, pc.lod);
}
