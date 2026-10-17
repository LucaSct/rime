// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the descriptor-cache proof (tests/rhi/descriptor_cache_test.cpp). Two
// uniform blocks at bindings 1 and 3 — the two lowest-numbered uniform buffers, so the Vulkan
// backend promotes BOTH to dynamic — plus a sampled texture at binding 2 between them. Each output
// channel comes from exactly ONE binding: red from binding 1, green from binding 3, blue from the
// texture. So a swapped dynamic offset shows as swapped red/green, a stale cached set as a wrong
// channel, and a wrong sampler as a different blue. The texture is sampled at u = 0.6 of a
// 2-texel-wide image: NEAREST returns texel 1, LINEAR returns 0.3*texel0 + 0.7*texel1.
#version 450

layout(set = 0, binding = 1) uniform BlockA {
    vec4 v;
} a;

layout(set = 0, binding = 3) uniform BlockB {
    vec4 v;
} b;

layout(set = 0, binding = 2) uniform sampler2D tex;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(a.v.x, b.v.x, texture(tex, vec2(0.6, 0.5)).b, 1.0);
}
