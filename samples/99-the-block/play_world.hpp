// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "rime/assets/procedural_terrain.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/world.hpp"
#include "rime/render/terrain_pass.hpp"
#include "rime/render/terrain_residency.hpp"
#include "rime/render/terrain_scene.hpp"
#include "rime/terrain_collision/procedural_source.hpp"

namespace rime::block_play {

// One bounded procedural world shared by the two simulation peers and, when present, the GPU.
// All methods run on the main thread between physics steps. The renderer borrows draws() only
// during synchronous graph declaration.
class PlayWorld {
public:
    static constexpr float kPracticalLimit = 8192.0f;
    static constexpr float kRecoveryFloor = -54.0f;
    static constexpr std::uint32_t kMaxGpuSlots = 640;
    static constexpr std::uint64_t kMaxGpuBytes = 96ull * 1024ull * 1024ull;

    struct CollisionPeer {
        physics::PhysicsWorld& physics;
        terrain_collision::ProceduralTileSource source;
        terrain_collision::TerrainCollision collision;
        terrain_collision::PinToken block_pin{};
        std::uint64_t barriers = 0;
        std::uint64_t failures = 0;

        CollisionPeer(const assets::ProceduralTerrainConfig& config,
                      const physics::Aabb& block,
                      physics::PhysicsWorld& physics);
        [[nodiscard]] bool prepare(ecs::World& world,
                                   destruction::DestructionWorld& destruction,
                                   std::uint64_t tick,
                                   core::Vec3 player,
                                   float max_speed);
    };

    struct Stats {
        std::uint32_t resident = 0;
        std::uint32_t peak_resident = 0;
        std::uint64_t gpu_bytes = 0;
        std::uint64_t peak_gpu_bytes = 0;
        std::uint64_t uploads = 0;
        std::uint64_t evictions = 0;
        std::uint64_t refusals = 0;
        std::uint64_t recenters = 0;
        std::uint64_t recoveries = 0;
    };

    PlayWorld(ecs::World& server_world,
              physics::PhysicsWorld& server_physics,
              physics::PhysicsWorld& client_physics,
              rhi::Device* device);
    ~PlayWorld();
    PlayWorld(const PlayWorld&) = delete;
    PlayWorld& operator=(const PlayWorld&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] bool prepare(ecs::World& server_world,
                               destruction::DestructionWorld& server_destruction,
                               ecs::World& client_world,
                               destruction::DestructionWorld& client_destruction,
                               std::uint64_t tick,
                               core::Vec3 server_player,
                               core::Vec3 client_player,
                               float max_speed);
    [[nodiscard]] bool recenter(core::Vec3 eye);
    void mark_drawn() noexcept;

    // Set before the first recenter. Textures remain owned by the scene's environment.
    void set_surface_palette(const render::TerrainPalette& palette) {
        surface_palette_ = palette;
        dressed_surface_ = true;
    }

    [[nodiscard]] render::TerrainPass* pass() noexcept { return pass_.get(); }

    [[nodiscard]] std::span<const render::TerrainSceneDraw> draws() const noexcept {
        return draws_;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] const assets::ProceduralTerrainConfig& config() const noexcept { return config_; }

    [[nodiscard]] CollisionPeer& server() noexcept { return server_; }

    [[nodiscard]] CollisionPeer& client() noexcept { return client_; }

    void note_recovery() noexcept { ++stats_.recoveries; }

private:
    struct Key {
        std::int32_t x = 0;
        std::int32_t z = 0;
        std::uint32_t lod = 0;
        friend bool operator==(const Key&, const Key&) = default;
    };

    struct Held {
        Key key{};
        render::TerrainResidentId slot{};
        render::TerrainTileId tile = render::kInvalidTerrainTile;
        std::uint64_t bytes = 0;
    };

    [[nodiscard]] static assets::ProceduralTerrainConfig street_config(ecs::World& world,
                                                                       bool& valid);
    [[nodiscard]] static physics::Aabb street_bounds(const assets::ProceduralTerrainConfig& cfg);
    [[nodiscard]] std::vector<Key> wanted(core::Vec3 eye) const;
    [[nodiscard]] Held* find(const Key& key) noexcept;

    bool valid_ = false;
    assets::ProceduralTerrainConfig config_{};
    CollisionPeer server_;
    CollisionPeer client_;
    rhi::Device* device_ = nullptr;
    std::unique_ptr<render::TerrainPass> pass_;
    render::TerrainSlotTable slots_{kMaxGpuSlots};
    std::array<render::TerrainTileId, kMaxGpuSlots> slot_tiles_{};
    std::vector<Held> held_; // never larger than 304; retained capacity across recenters
    std::vector<render::TerrainSceneDraw> draws_;
    render::TerrainFrame frame_ = 0;
    Stats stats_{};
    render::TerrainPalette surface_palette_{};
    bool dressed_surface_ = false;
};

[[nodiscard]] bool needs_recovery(core::Vec3 position) noexcept;

} // namespace rime::block_play
