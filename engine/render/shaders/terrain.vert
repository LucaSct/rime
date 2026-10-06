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
//
// ── m19.8d2: GEOMORPHING ONTO THE PARENT (ADR-0071) ──────────────────────────────────────────
//
// A tile of a LOD chain (ADR-0070) shares every second sample with its parent: child sample
// (2I, 2J) IS parent sample (I, J), bit for bit. The child's other ("odd") vertices are the extra
// detail. Where does each one sit on the PARENT's surface? The parent splits cell (I, J) along
// ADR-0060's fixed diagonal, (I, J)→(I+1, J+1). So:
//
//   (odd, even)  lies on the parent cell edge between its two even neighbours along x;
//   (even, odd)  lies on the parent cell edge between its two even neighbours along z;
//   (odd, odd)   lies on the parent cell DIAGONAL, between (i−1, j−1) and (i+1, j+1) — the
//                min-to-max pair. With the other diagonal it would be (i+1, j−1) and (i−1, j+1).
//
// Each is the MIDPOINT of its segment in x and z, and the parent's surface along a triangle edge
// is linear, so the parent's height there is the AVERAGE of the two even neighbours' heights. No
// fetch of the parent tile is needed: the child already holds the parent's vertices.
//
// And at morph 1 the child mesh IS the parent surface, not an approximation of it: every child
// triangle (the same diagonal direction, at half the size) has its three vertices inside ONE
// parent triangle — on its corners, edges or diagonal — so with those vertices on the parent's
// plane, the child triangle lies in that plane. That is what makes a switch between a fully
// morphed child and its parent invisible: the same surface, drawn with more triangles.
//
// The morph factor is computed per VERTEX from its world position (before morphing) and the
// selection's camera, never per tile. A vertex on the edge two tiles share is drawn by both, and
// both must move it identically or the edge cracks; per vertex, from the same global sample index
// and the same integer height, they compute the same f32 inputs and so the same morph. An edge
// whose neighbour is drawn COARSER (lod_tile.z bit) is forced to morph 1: its odd vertices land on
// the coarse neighbour's edge, a T-junction with no gap beyond the f32 rounding of the average.
//
// Only y moves — each odd vertex already sits at its segment's midpoint in x and z — so the
// fragment stage's derivative normal is the plane of the morphed triangle at every morph, and at
// morph 1 it is the parent triangle's own normal.

#version 450

// Binding 0: the tile's heights, one R16_UNORM texel per sample, row-major with x fastest — the
// same walk `HeightfieldAsset::samples` already is, which is why the upload is a memcpy.
layout(set = 0, binding = 0) uniform sampler2D heightfield;

// 208 bytes (m19.8d2) — above the 128 Vulkan guarantees, so TerrainPass checks the device's limit.
// Mirrored by `TerrainPush` in terrain_pass.hpp, which static_asserts the size; build it with
// `terrain_push()` rather than by hand so a second caller (the m19.3 proof builds its own pipeline
// around this very stage) cannot assemble a differently-shaped block.
layout(push_constant) uniform Pc {
    mat4 view_proj;  //   0..63  clip-from-world
    vec4 placement;  //  64..79  xyz = the tile's world origin, w = height_offset (metres at q=0)
    vec4 grid;       //  80..95  x = cell_size_x, y = cell_size_z, z = height_scale, w = columns
    vec4 sun;        //  96..111 xyz = unit direction the light travels, w = irradiance (fragment)
    vec4 surface;    // 112..127 rgb = albedo, w = ambient (fragment)
    vec4 eye;        // 128..143 xyz = camera world position (fragment)
    vec4 material;   // 144..159 x = metallic, y = roughness (fragment, flat tiles)
    vec4 lod_origin; // 160..175 xyz = world grid origin, w = morph start (m)       [m19.8d2]
    vec4 lod_camera; // 176..191 xyz = the selection's camera, w = 1/(end − start)  [m19.8d2]
    ivec4 lod_tile;  // 192..207 base sample index x, z; flags; level              [m19.8d2]
} pc;

// World position, interpolated. The fragment stage needs it for shading, and the proof needs its
// .y — the reconstructed world height, which is the number ADR-0062's structural test compares
// against `rime::physics`.
layout(location = 0) out vec3 v_world;

// Tile-LOCAL xz in metres (i * cell_x, j * cell_z), exact — the splat weight map is addressed in the
// tile's own frame (m19.4, ADR-0063), and recovering it in the fragment stage as `v_world - origin`
// would reintroduce a rounding the vertex stage does not have to pay.
layout(location = 1) out vec2 v_local;

const int kLodEnabled = 16; // kTerrainPushLodEnabled

// m19.8d2: the dequantised height of sample (i, j) in WORLD metres, for the LOD path.
//
// `round` recovers the stored integer exactly from the R16_UNORM fetch (q / 65535 · 65535 can land
// a fraction of an ULP off q), and `precise` forbids fusing the multiply-add, so the result is the
// IEEE f32 evaluation of ADR-0070's engine order  origin.y + (offset + scale · q)  — the same bits
// the CPU computes. That is what lets the morph-0 proof compare vertex heights BIT for bit.
float lod_height(int i, int j) {
    const float q = round(texelFetch(heightfield, ivec2(i, j), 0).r * 65535.0);
    precise float local = pc.placement.w + pc.grid.z * q;
    precise float world = pc.lod_origin.y + local;
    return world;
}

void main() {
    // `columns` travels as a float. An f32 represents every integer below 2^24 exactly and
    // ADR-0060 caps a heightfield at 32768 samples per axis, so the round trip is lossless; it
    // buys the 16 bytes that let the fragment stage's light live in the same 128-byte block.
    const uint columns = uint(pc.grid.w);
    const uint i = uint(gl_VertexIndex) % columns; // along local +X
    const uint j = uint(gl_VertexIndex) / columns; // along local +Z

    // One pixel when the proofs draw the grid as POINTS (each vertex read back on its own); a
    // triangle list ignores it.
    gl_PointSize = 1.0;
    v_local = vec2(float(i) * pc.grid.x, float(j) * pc.grid.y);

    if ((pc.lod_tile.z & kLodEnabled) == 0) {
        // ── The m19.3 path, unchanged: a tile on its own, placed by its own origin. ──────────
        // ADR-0060 §1's dequantisation, unchanged: height = offset + scale * q.
        const float q = texelFetch(heightfield, ivec2(i, j), 0).r * 65535.0;
        const float height = pc.placement.w + pc.grid.z * q;

        // Translation only. A `HeightfieldAsset` carries an `origin` and no rotation, so a yawed tile
        // (which the PHYSICS store does allow) is not expressible by the asset this pass draws — see
        // ADR-0062's deferred list rather than inventing a convention here.
        const vec3 world = pc.placement.xyz +
                           vec3(float(i) * pc.grid.x, height, float(j) * pc.grid.y);

        v_world = world;
        gl_Position = pc.view_proj * vec4(world, 1.0);
        return;
    }

    // ── m19.8d2: one node of a LOD chain (see the header). ──────────────────────────────────
    const int ii = int(i);
    const int jj = int(j);
    const int last = int(columns) - 1; // tiles of a world are square (TerrainWorldGrid::samples)

    // Placed by GLOBAL sample index, so a vertex two tiles share is bit-identical in both.
    const ivec2 g = pc.lod_tile.xy + ivec2(ii, jj);
    precise vec3 world;
    world.x = pc.lod_origin.x + float(g.x) * pc.grid.x;
    world.z = pc.lod_origin.z + float(g.y) * pc.grid.y;
    world.y = lod_height(ii, jj);

    // The morph factor, from THIS vertex's world position (unmorphed) and the selection's camera.
    const float d = length(world - pc.lod_camera.xyz);
    float m = clamp((d - pc.lod_origin.w) * pc.lod_camera.w, 0.0, 1.0);
    const int edges = pc.lod_tile.z;
    if (((edges & 1) != 0 && ii == 0) || ((edges & 2) != 0 && ii == last) ||
        ((edges & 4) != 0 && jj == 0) || ((edges & 8) != 0 && jj == last)) {
        m = 1.0; // the neighbour across this edge is coarser: sit exactly on its edge
    }

    const bool odd_i = (ii & 1) != 0;
    const bool odd_j = (jj & 1) != 0;
    if (odd_i || odd_j) {
        // The two even neighbours on the parent segment this vertex is the midpoint of.
        ivec2 a;
        ivec2 b;
        if (odd_i && odd_j) {
            a = ivec2(ii - 1, jj - 1); // the parent's diagonal: ADR-0060's min-to-max
            b = ivec2(ii + 1, jj + 1);
        } else if (odd_i) {
            a = ivec2(ii - 1, jj);
            b = ivec2(ii + 1, jj);
        } else {
            a = ivec2(ii, jj - 1);
            b = ivec2(ii, jj + 1);
        }
        precise float target = 0.5 * (lod_height(a.x, a.y) + lod_height(b.x, b.y));
        // (1 − m)·own + m·target, not mix(): the endpoints are then exact on every GPU — at m = 0
        // the second term is +0 and at m = 1 the first is — where a + (b − a)·m need not return b.
        precise float y = (1.0 - m) * world.y + m * target;
        world.y = y;
    }

    v_world = world;
    gl_Position = pc.view_proj * vec4(world, 1.0);
}
