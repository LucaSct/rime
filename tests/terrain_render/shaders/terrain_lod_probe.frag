// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m19.8d3 proof probe: reports what terrain.vert handed the fragment stage in v_lod — the vertex's
// sample coordinate and the MORPH FACTOR it was moved by — so the proof can check that a vertex
// two tiles share gets ONE factor whichever tile draws it. Drawn as points, one pixel per vertex,
// so nothing is interpolated: the words are the vertex stage's own f32 bits.
#version 450

layout(location = 2) in vec3 v_lod;

layout(location = 0) out uint out_morph_bits;

void main() {
    // The sign bit marks "a point landed here": m is never negative, so the word is never 0.
    out_morph_bits = floatBitsToUint(v_lod.z) ^ 0x80000000u;
}
