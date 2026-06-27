// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Fragment shader for the 3-D-texture proof (ADR-0013). It samples a sampler3D down its depth (w)
// axis using the screen's vertical coordinate, holding u,v at the centre of the single x,y texel.
// With a 1×1×2 volume (red slice at w=0, green slice at w=1) and nearest filtering, the top half of
// the image reads red and the bottom half green — proving the 3-D image/view/upload and sampler3D path.
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler3D u_volume;

void main() {
    out_color = vec4(texture(u_volume, vec3(0.5, 0.5, v_uv.y)).rgb, 1.0);
}
