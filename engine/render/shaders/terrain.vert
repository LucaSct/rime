// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The terrain heightfield pass, vertex stage (m19.3, ADR-0062). This is the ONLY place in the
// engine that turns a cooked heightfield into geometry, and that is the entire point of the brick.
//
// ── WHY THE HEIGHT IS FETCHED HERE RATHER THAN BAKED INTO A VERTEX BUFFER ────────────────────
//
// The obvious implementation of "draw a heightfield" is to walk the samples on the CPU, build a
// vertex buffer of float positions, and upload it. That would work, and it would quietly create a
// SECOND representation of the terrain surface: one in physics' u16 store, one in a float vertex
// buffer, each with its own rounding and its own chance to be rebuilt from stale data. ADR-0060 §1
// chose u16 samples precisely so that would not have to happen — "the renderer can upload the
// samples as R16_UNORM and apply the same scale/offset, so the surface drawn and the surface
// collided with come from the same integers".
//
// So this shader is a VERTEX PULLER: the draw has no vertex buffer at all. `gl_VertexIndex` is a
// grid coordinate, the height comes out of the heightfield texture, and the XZ position is
// arithmetic on the grid spacing. There is nothing to keep in sync, because there is nothing else.
//
// ── THE DEQUANTISATION, AND WHY texelFetch ───────────────────────────────────────────────────
//
// The texture is R16_UNORM, so a fetch returns `q / 65535` in [0, 1]; multiplying by 65535 gives
// the stored integer `q` back, and `height_offset + height_scale * q` is ADR-0060's dequantisation
// verbatim — the SAME expression `HeightfieldShape::h()` evaluates on the CPU
// (engine/physics/src/heightfield.hpp).
//
// `texelFetch` with integer coordinates, NOT a normalised `texture()` sample: a filtered sample
// would blend neighbouring samples by whatever the sampler is configured to do and would need
// half-texel-correct UVs to land on a sample at all. A vertex of the grid IS a sample; fetching it
// by index is exact, is immune to a sampler misconfiguration, and is the reason the proof's margin
// can be derived from the unorm round trip alone (tests/terrain_render/terrain_height_test.cpp).
#version 450

// Binding 0: the tile's heights, one R16_UNORM texel per sample, row-major with x fastest — the
// same walk `HeightfieldAsset::samples` already is, which is why the upload is a memcpy.
layout(set = 0, binding = 0) uniform sampler2D heightfield;

// 128 bytes exactly — the push-constant size every Vulkan implementation guarantees. Mirrored by
// `TerrainPush` in terrain_pass.hpp, which static_asserts the size; build it with
// `terrain_push()` rather than by hand so a second caller (the m19.3 proof builds its own pipeline
// around this very stage) cannot assemble a differently-shaped block.
layout(push_constant) uniform Pc {
    mat4 view_proj;  //   0..63  clip-from-world
    vec4 placement;  //  64..79  xyz = the tile's world origin, w = height_offset (metres at q=0)
    vec4 grid;       //  80..95  x = cell_size_x, y = cell_size_z, z = height_scale, w = columns
    vec4 sun;        //  96..111 xyz = unit direction the light travels, w = irradiance (fragment)
    vec4 surface;    // 112..127 rgb = albedo, w = ambient (fragment)
} pc;

// World position, interpolated. The fragment stage needs it for shading, and the proof needs its
// .y — the reconstructed world height, which is the number ADR-0062's structural test compares
// against `rime::physics`.
layout(location = 0) out vec3 v_world;

// Tile-LOCAL xz in metres (i * cell_x, j * cell_z), exact — the splat weight map is addressed in the
// tile's own frame (m19.4, ADR-0063), and recovering it in the fragment stage as `v_world - origin`
// would reintroduce a rounding the vertex stage does not have to pay.
layout(location = 1) out vec2 v_local;

void main() {
    // `columns` travels as a float. An f32 represents every integer below 2^24 exactly and
    // ADR-0060 caps a heightfield at 32768 samples per axis, so the round trip is lossless; it
    // buys the 16 bytes that let the fragment stage's light live in the same 128-byte block.
    const uint columns = uint(pc.grid.w);
    const uint i = uint(gl_VertexIndex) % columns; // along local +X
    const uint j = uint(gl_VertexIndex) / columns; // along local +Z

    // ADR-0060 §1's dequantisation, unchanged: height = offset + scale * q.
    const float q = texelFetch(heightfield, ivec2(i, j), 0).r * 65535.0;
    const float height = pc.placement.w + pc.grid.z * q;

    // Translation only. A `HeightfieldAsset` carries an `origin` and no rotation, so a yawed tile
    // (which the PHYSICS store does allow) is not expressible by the asset this pass draws — see
    // ADR-0062's deferred list rather than inventing a convention here.
    const vec3 world = pc.placement.xyz +
                       vec3(float(i) * pc.grid.x, height, float(j) * pc.grid.y);

    v_world = world;
    v_local = vec2(float(i) * pc.grid.x, float(j) * pc.grid.y);
    gl_Position = pc.view_proj * vec4(world, 1.0);
}
