// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// HUD overlay vertex stage (m13.3b). Positions arrive already in clip space — the CPU does the
// pixels-to-NDC conversion once per vertex rather than the shader doing it per vertex from a
// uniform, which keeps this stage free of any per-frame binding at all.
#version 450

layout(location = 0) in vec2 in_pos;     // clip space, xy
// uv.xy plus the draw mode in .z — the mode shares this attribute because the RHI has no scalar
// float vertex format, and a single per-vertex flag is not worth growing one.
layout(location = 1) in vec3 in_uv_mode; // 0 = solid fill, 1 = distance-field glyph
layout(location = 2) in vec4 in_color;   // linear RGBA

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_mode;

void main() {
    v_uv = in_uv_mode.xy;
    v_color = in_color;
    v_mode = in_uv_mode.z;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
