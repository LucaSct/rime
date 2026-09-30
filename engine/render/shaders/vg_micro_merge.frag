// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M18.4: resolve the software micro-triangle buffer into the SAME visibility target the hardware
// pass wrote (ADR-0056). One fullscreen triangle; each fragment reads its pixel's 64-bit
// (depth key, list index) word from vg_micro_raster.comp.
//
// The hardware/software depth arbitration is not written here — it is the DEPTH TEST. This pass
// loads the hardware pass's depth attachment, writes the software depth through gl_FragDepth, and
// compares with Less. So the software sample replaces the hardware one exactly when it is strictly
// nearer, and an exact tie keeps the hardware pixel: the hybrid frame behaves as if every
// hardware triangle were drawn before every software one, which is the order the proofs use for
// the single-path reference. Reusing the fixed-function test also keeps the depth attachment
// itself correct for any later pass, rather than leaving it hardware-only.
//
// No fragment-stage writes to storage (which would need fragmentStoresAndAtomics): the buffer is
// only read, and the attachments carry the result.
#version 450

struct MicroTriangle {
    float x0, y0, x1, y1, x2, y2;
    float z0, z1, z2;
    uint id_lo;
    uint id_hi;
    uint pad;
};

layout(std430, set = 0, binding = 0) readonly buffer Triangles {
    MicroTriangle triangles[];
};

// Read as two words per pixel whichever raster variant wrote it: .x = list index, .y = depth key.
layout(std430, set = 0, binding = 1) readonly buffer Samples {
    uvec2 samples[];
};

layout(push_constant) uniform Pc {
    uint width;
} pc;

layout(location = 0) out uvec2 out_visibility; // RG32Uint: v3 ID (.x lo, .y hi)
layout(location = 1) out uint out_depth_bits;  // floatBitsToUint(depth), as vg_visibility.frag

// Inverse of vg_micro_raster.comp's depth_key.
float key_depth(uint key) {
    return uintBitsToFloat((key & 0x80000000u) != 0u ? (key & 0x7FFFFFFFu) : ~key);
}

void main() {
    const uvec2 p = uvec2(gl_FragCoord.xy);
    const uvec2 s = samples[p.y * pc.width + p.x];
    if (s.y == 0xFFFFFFFFu) {
        discard; // the clear value: no software triangle covered this pixel
    }
    const float depth = key_depth(s.y);
    const MicroTriangle t = triangles[s.x];
    gl_FragDepth = depth;
    out_visibility = uvec2(t.id_lo, t.id_hi);
    out_depth_bits = floatBitsToUint(depth);
}
