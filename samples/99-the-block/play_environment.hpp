// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/assets/procedural_terrain.hpp"
#include "rime/blockkit/palette.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/terrain_pass.hpp"

namespace rime::block_play {

// Sample-owned art direction. Every texture/mesh is generated once from fixed seeds at setup;
// ordinary frames only draw ECS instances. This is content, not a second terrain simulation.
// The texture owner must outlive both renderers and all resident terrain tiles borrowing it.
class PlayEnvironment {
public:
    explicit PlayEnvironment(rhi::Device& device) : device_(device) {}

    ~PlayEnvironment();
    PlayEnvironment(const PlayEnvironment&) = delete;
    PlayEnvironment& operator=(const PlayEnvironment&) = delete;

    [[nodiscard]] bool build(ecs::World& world,
                             render::MeshRegistry& meshes,
                             render::MaterialRegistry& materials,
                             const blockkit::BlockPalette& block,
                             const assets::ProceduralTerrainConfig& config);

    [[nodiscard]] const render::TerrainPalette& terrain_palette() const noexcept {
        return terrain_;
    }

    struct Stats {
        unsigned trees = 0;
        unsigned grass_tufts = 0;
        unsigned puddles = 0;
        unsigned ponds = 0;
        unsigned wall_materials = 0;
        unsigned textures = 0;
    };

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] render::MaterialId water_material() const noexcept { return water_; }

    // The hidden skirt between the city plate's edge and the terrain's low "street" rectangle.
    //
    // THE BUG THIS FIXES (owner: "I cannot jump the vertical gap between the outer and inner
    // terrain"). The procedural terrain is -1.5 m INSIDE its street rectangle and climbs back to
    // 0 over the next 12 m. The rectangle used to be the plate's own footprint, so the terrain sat
    // 1.5 m BELOW the plate top at the plate edge: a player walking in from the flat outer band
    // descended a bowl and met a 1.5 m wall. The step-up ladder climbs 0.3 m and a jump apex is
    // ~1.0 m, so the plate could not be entered at all.
    //
    // The terrain only has to be below the plate where it is HIDDEN under the plate slab, so the
    // street rectangle is shrunk by this many metres: the whole 12 m climb then happens under the
    // plate and the terrain arrives at ground level (= the plate top) exactly at the plate edge,
    // with the smoothstep's zero slope there. One height function still serves collision and
    // rendering, so the walkable surface and the drawn surface cannot disagree. It must be >= the
    // terrain's street->ground blend (kStreetBlendEnd = 12 m in engine/assets/src/
    // procedural_terrain.cpp); the plate-boundary proof (`--world-smoke`) fails if it is not.
    static constexpr std::int32_t kPlateSkirt = 12;

    // Same global-coordinate weight function at every LOD. Height samples remain untouched:
    // the drawn grass/dirt cannot change what either physics peer collides with.
    static void paint_terrain(assets::HeightfieldAsset& asset,
                              const assets::ProceduralTerrainConfig& config);

private:
    rhi::Device& device_;
    std::vector<rhi::TextureHandle> textures_;
    render::TerrainPalette terrain_{};
    Stats stats_{};
    render::MaterialId water_ = render::kInvalidMaterialId;
};

} // namespace rime::block_play
