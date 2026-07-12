// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the textured quad: it samples the bound texture at the interpolated UV and
// outputs that color. The `sampler2D` is a *combined image-sampler* at descriptor set 0, binding 0 —
// the binding the RHI's M3.5 descriptor model fills in via CommandBuffer::bind_texture (the pipeline
// was created with GraphicsPipelineDesc::sampled_texture = true, which declares this layout).
#version 450

layout(location = 0) in vec2 frag_uv;     // interpolated from the vertex shader
layout(location = 0) out vec4 out_color;  // -> the single color attachment (location 0)

layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main() {
    out_color = texture(u_texture, frag_uv);
}
