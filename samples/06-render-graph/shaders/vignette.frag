// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The demo post pass for 06-render-graph: a vignette. It reads the tonemapped image and darkens it
// toward the edges — a deliberately trivial effect, because the point of the sample is that ADDING
// this pass to the frame is ~10 lines of C++ (one add_raster_pass call), not the effect itself.
// texelFetch by gl_FragCoord reads exactly the matching source texel (no filtering, no uv math);
// textureSize supplies the resolution so the falloff needs no uniform.
#version 450

layout(set = 0, binding = 0) uniform sampler2D src;

layout(location = 0) out vec4 out_color;

void main() {
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const vec3 c = texelFetch(src, p, 0).rgb;
    const vec2 uv = (vec2(p) + 0.5) / vec2(textureSize(src, 0));
    // Distance from center, 0 at the middle to ~0.707 at the corners; fade from full brightness
    // inside 0.35 to black by 0.75 so the corners darken but the subject stays lit.
    const float v = smoothstep(0.75, 0.35, length(uv - 0.5));
    out_color = vec4(c * v, 1.0);
}
