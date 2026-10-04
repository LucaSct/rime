// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/render_graph.hpp"

// The terrain heightfield DRAW pass (m19.3, ADR-0062-m19.3-terrain-render).
//
// M19.1 cooked the heightfield (ADR-0060: a regular grid of u16 samples plus a scale and an
// offset) and gave `rime::physics` a shape that collides with it. Nothing drew it. This pass is
// the pixels, and it is built around one hard requirement:
//
//   ── THE DRAWN SURFACE AND THE COLLIDED SURFACE COME FROM IDENTICAL INTEGERS ──
//
// Not "from the same asset", not "from the same numbers to within a tolerance" — from the same
// u16 samples, dequantised by the same `offset + scale * q`. The failure this prevents is the one
// every engine with a terrain has shipped at least once: a ball that visibly floats above, or
// sinks into, ground that is drawn a few centimetres off the ground that is simulated. ADR-0060 §1
// already named the mechanism; m19.3 is where it is actually built and measured.
//
// The three things that make it true, each verifiable by reading the code:
//
//  1. **The samples are uploaded VERBATIM, as R16_UNORM** (`rhi::Format::R16Unorm`, added for this
//     brick). `upload()` memcpys `HeightfieldAsset::samples` into the texture. No float ever
//     touches a height on the way to the GPU, so there is no conversion to get wrong.
//  2. **The vertex stage reconstructs the height from that texture** (terrain.vert — a vertex
//     puller; the draw binds no vertex buffer at all), applying `height_offset + height_scale * q`
//     — character for character the expression `HeightfieldShape::h()` evaluates on the CPU.
//  3. **The triangulation is ADR-0060's, taken from the asset's own header field.** Each cell is
//     split along its (i,j)→(i+1,j+1) diagonal, triangle A = (v00, v10, v11) then B = (v00, v11,
//     v01). A non-planar cell has two different surfaces depending on the split, so this is not a
//     stylistic choice: get it wrong and the drawn and collided surfaces differ by the cell's
//     non-planarity even though every sample agrees. `upload()` REFUSES a triangulation value it
//     does not know rather than guessing.
//
// The proof is `tests/terrain_render/terrain_height_test.cpp`: at 36 positions spread over a
// non-planar tile, the height this pass's vertex stage reconstructs on the GPU is compared with
// the height a `rime::physics` raycast lands on, against a margin derived from the R16_UNORM round
// trip. Structural, no golden images (the M5.6/M6.4 pattern).
//
// ── OWNERSHIP AND MODULARITY ─────────────────────────────────────────────────────────────────
//
// Owned exactly like `FxParticlePass`: a standalone pass object a caller composes into its frame,
// not something `SceneRenderer` reaches into. Nothing else in `rime_render` includes this header,
// so deleting these four files and their two CMake lines leaves the engine building — the
// modularity guardrail, satisfied structurally rather than by promise.
//
// ── WHAT THIS PASS IS NOT, YET (all deferred in ADR-0062, none of it silent) ──────────────────
//
//   * no LOD, no clipmap, no tessellation: a tile is drawn at full sample density, every frame;
//   * no splat blending — one flat albedo, see terrain.frag;
//   * no streaming: `upload()` is a one-shot, and a tile stays resident until the pass dies;
//   * no holes, no decals, no per-cell best-fit diagonals (the format cannot express them either);
//   * a tile is placed by TRANSLATION only, because `HeightfieldAsset` carries an `origin` and no
//     rotation — physics can yaw a registered tile, the cooked asset cannot say so.
namespace rime::assets {
struct HeightfieldAsset;
}

namespace rime::render {

// A tile handle, dense and 0-based — the MeshRegistry convention (mesh.hpp), for the same reason:
// a caller holds an id, never GPU handles.
using TerrainTileId = std::uint32_t;
inline constexpr TerrainTileId kInvalidTerrainTile = 0xFFFFFFFFu;

// The largest tile this pass will upload, per axis. ADR-0060 caps a heightfield at 32768 samples
// per axis, which is far beyond what one DRAW should be: the index buffer for a grid is
// 6·(columns−1)·(rows−1) u32s, so a 32768² tile would ask for 25 GB of indices. 1024² asks for
// 25 MB, which is already generous for a tile — ADR-0060 §2's scaling answer is MANY TILES, not
// one enormous one, so a tile past this bound is a cook mistake and is refused and counted.
inline constexpr std::uint32_t kMaxTileSamplesPerAxis = 1024;

// One terrain tile's GPU residency. Public because the m19.3 proof builds a second pipeline around
// this pass's own vertex stage and needs the very resources the pass draws with — handing it these
// is what keeps the proof measuring the ENGINE's path instead of a copy of it.
struct TerrainTile {
    rhi::TextureHandle heights{}; // R16_UNORM, columns x rows, the cooked samples verbatim
    rhi::BufferHandle indices{};  // 6 per cell, ADR-0060's diagonal, u32
    std::uint32_t index_count = 0;
    std::uint32_t vertex_count = 0; // columns * rows — the grid the vertex stage indexes
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    float cell_size_x = 0.0f;
    float cell_size_z = 0.0f;
    float height_scale = 0.0f;
    float height_offset = 0.0f;
    core::Vec3 origin{0.0f, 0.0f, 0.0f};
};

// How the tile is lit. A struct rather than four arguments so a caller cannot transpose them, and
// deliberately NOT read out of the ECS: this pass is composed by its caller, which already knows
// the frame's sun (and `SceneRenderer`'s extraction is the right source once terrain joins a
// scene — ADR-0062's deferred list).
struct TerrainLight {
    core::Vec3 sun_direction{0.0f, -1.0f, 0.0f}; // the direction light TRAVELS; normalised by add()
    float sun_irradiance = 3.0f;                 // W/m² on a surface facing the sun
    core::Vec3 albedo{0.35f, 0.33f, 0.28f};      // a plausible dirt/grass grey-green
    float ambient = 0.05f;                       // flat irradiance, stands in for sky + bounce
};

// The push block both terrain shaders read, byte for byte. 128 bytes — the floor every Vulkan
// implementation guarantees, so no capability check. Build it with `terrain_push()`.
struct TerrainPush {
    core::Mat4 view_proj;
    float placement[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz = tile origin, w = height_offset
    float grid[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // cell_x, cell_z, height_scale, columns
    float sun[4] = {0.0f, -1.0f, 0.0f, 0.0f};      // xyz = travel direction, w = irradiance
    float surface[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // rgb = albedo, w = ambient
};

static_assert(sizeof(TerrainPush) == 128,
              "TerrainPush must match terrain.vert / terrain.frag's push_constant block");

// Fold a tile + a view + a light into the shader's constant block. Free and public on purpose:
// the pass uses it, and so does the proof's own pipeline, so the two cannot drift. A zero-length
// `sun_direction` falls back to straight down rather than producing a NaN normal.
[[nodiscard]] TerrainPush
terrain_push(const TerrainTile& tile, const core::Mat4& view_proj, const TerrainLight& light);

class TerrainPass {
public:
    explicit TerrainPass(rhi::Device& device);
    ~TerrainPass();

    TerrainPass(const TerrainPass&) = delete;
    TerrainPass& operator=(const TerrainPass&) = delete;

    // Make a cooked heightfield resident: the samples as an R16_UNORM texture, plus the grid's
    // index buffer. Returns `kInvalidTerrainTile` (and bumps `tiles_refused()`) for an asset this
    // pass will not draw — a grid smaller than one cell, a sample span that does not match the
    // grid, a non-finite or non-positive spacing/scale, an axis past `kMaxTileSamplesPerAxis`, or
    // a `triangulation` value this build does not know. It REFUSES rather than repairing, the
    // registration posture ADR-0060 §2 set for the physics side.
    [[nodiscard]] TerrainTileId upload(const assets::HeightfieldAsset& asset);

    // UNCHECKED, the MeshRegistry::get contract: `id` must have come from a successful upload().
    [[nodiscard]] const TerrainTile& tile(TerrainTileId id) const { return tiles_[id]; }

    [[nodiscard]] bool contains(TerrainTileId id) const noexcept {
        return id != kInvalidTerrainTile && id < tiles_.size();
    }

    [[nodiscard]] std::size_t tile_count() const noexcept { return tiles_.size(); }

    // Declare the terrain draw into `hdr` (loaded, not cleared — terrain joins a frame other
    // passes have already contributed to) with `depth` written, like any other opaque geometry.
    //
    // A NO-OP when `id` is not a tile this pass holds: no pipeline bind, no barrier, no attachment
    // load, so a frame without terrain is byte-identical to a build without this file in it — the
    // structural gate discipline ADR-0032 §11 set and `FxParticlePass` follows.
    void add(RenderGraph& graph,
             RGTexture hdr,
             RGTexture depth,
             TerrainTileId id,
             const core::Mat4& view_proj,
             const TerrainLight& light);

    // Assets `upload()` would not draw. Guardrail 5: a refused tile and a tile that was never
    // handed over produce the same empty frame, so the difference has to be countable.
    [[nodiscard]] std::uint64_t tiles_refused() const noexcept { return refused_; }

    // Tiles actually submitted, cumulative — the vacuity witness. "The pass drew nothing" and
    // "the pass drew a tile you cannot see" look identical on screen.
    [[nodiscard]] std::uint64_t tiles_drawn() const noexcept { return drawn_; }

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
    std::vector<TerrainTile> tiles_;
    std::uint64_t refused_ = 0;
    std::uint64_t drawn_ = 0;
};

} // namespace rime::render
