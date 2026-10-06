// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The m19.8d2 proof's fragment stage: the companion of terrain_height_probe.frag. Paired with the
// ENGINE's terrain.vert drawing a tile as POINTS (one pixel per vertex), it reports the world
// position that vertex stage placed the vertex at, as exact f32 bit patterns in two integer
// targets — y in an R32_UINT, x and z in an RG32_UINT. Integer targets are never blended, filtered
// or converted (see terrain_height_probe.frag), and a point's varyings are its one vertex's values,
// so nothing is interpolated either: these are the vertex's own bits.
//
// No vertex of the proof's worlds has y == 0 (the ground sits metres above the origin), so a pixel
// whose y bits are all zero is one no point landed on — the coverage witness.
#version 450

layout(location = 0) in vec3 v_world;

layout(location = 0) out uint out_y_bits;
layout(location = 1) out uvec2 out_xz_bits;

void main() {
    out_y_bits = floatBitsToUint(v_world.y);
    out_xz_bits = uvec2(floatBitsToUint(v_world.x), floatBitsToUint(v_world.z));
}
