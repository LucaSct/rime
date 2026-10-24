// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Snapshot of the finished opaque scene for the water pass (screen-space refraction).
//
// WHY A COPY. A pass cannot sample the image it is rendering into, and the water pass both draws
// into the scene colour/depth and has to read what lies BEHIND the water. The RHI has no
// texture-to-texture copy, so one fullscreen triangle does it: colour passes straight through, and
// depth is written as its raw IEEE bits into an R32_UINT target (the RHI offers no R32_SFLOAT; the
// bits round-trip exactly through uintBitsToFloat). Only the water pass reads either, and the pass
// is declared only on frames that actually draw water.
#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(location = 0) out vec4 out_color;
layout(location = 1) out uint out_depth_bits;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    out_color = texelFetch(scene_color, p, 0);
    out_depth_bits = floatBitsToUint(texelFetch(scene_depth, p, 0).r);
}
