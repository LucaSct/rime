// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The virtual-geometry visibility pass, fragment stage (M18 step 1). It pairs with pick_id.vert
// (same push-constant block: MVP + one uint), because a visibility buffer is the pick pass's ID
// idea applied to every pixel: no shading here, only "which cluster won the depth test". The id
// is the packed R32Uint visibility-ID ABI, already packed on the CPU.
//
// The second output carries window-space depth as raw float bits. Depth attachments cannot be
// read back through the RHI's colour-aspect copy, and there is no R32Float colour format, so the
// bit pattern travels in an integer target: floatBitsToUint is exact, and 0 (the clear) is +0.0.
#version 450

layout(location = 0) out uint out_visibility;
layout(location = 1) out uint out_depth_bits;

layout(push_constant) uniform Pc {
    mat4 mvp;
    uint id;
} pc;

void main() {
    out_visibility = pc.id;
    out_depth_bits = floatBitsToUint(gl_FragCoord.z);
}
