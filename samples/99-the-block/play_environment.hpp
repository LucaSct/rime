// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

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
