// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#version 450

layout(set = 0, binding = 0) uniform sampler2D world_depth;
layout(set = 0, binding = 1) uniform sampler2D model_depth;

void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float model = texelFetch(model_depth, pixel, 0).r;
    const float world = texelFetch(world_depth, pixel, 0).r;
    // The held model is always in front where it exists. min(model, world) would let a wall
    // occlude the model and would recreate the exact first-person clipping this pass prevents.
    gl_FragDepth = model < 1.0 ? model : world;
}
