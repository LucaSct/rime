// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#version 450
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform sampler2DArrayShadow shadow_map;
layout(std140, set = 0, binding = 1) uniform ShadowUniforms {
    mat4 cascade_view_proj[4];
    vec4 params;
    vec4 texel;
} shadow;
layout(push_constant) uniform Probe {
    vec4 sweep; // low, high, width, height
    ivec4 mode; // x = 0: x boundary, 1: depth boundary
} probe;
layout(location = 0) out vec4 out_color;
#include "../../../engine/render/shaders/sun_shadow.glsl"
void main() {
    float value = mix(probe.sweep.x, probe.sweep.y, gl_FragCoord.x / probe.sweep.z);
    vec3 world = probe.mode.x == 0 ? vec3(value, 0.0, 0.5) : vec3(0.0, 0.0, value);
    float visibility = sun_shadow(world, vec3(0.0, 1.0, 0.0));
    out_color = vec4(visibility, visibility, visibility, 1.0);
}
