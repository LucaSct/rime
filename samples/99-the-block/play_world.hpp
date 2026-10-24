// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#include "rime/assets/procedural_terrain.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/character.hpp"
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

    // The authored city plate (the one GroundSurface): its exact rectangle and walkable top, in
    // world metres. Kept beside the integer terrain rectangle because the two differ on purpose --
    // see PlayEnvironment::kPlateSkirt for why the terrain's low rectangle is the plate shrunk.
    struct PlateRect {
        float min_x = 0.0f;
        float max_x = 0.0f;
        float min_z = 0.0f;
        float max_z = 0.0f;
        float top_y = 0.0f;
    };

    // The result of `prove_plate_boundary`: every number the check judges, so a failing run says
    // which side and by how much, and a passing run can be shown to have looked (non-vacuity).
    struct PlateProof {
        unsigned samples = 0;   // perimeter points where a collision height was read
        unsigned uncovered = 0; // points where the collision tile could not be made resident
        unsigned no_hit = 0;    // points where a downward ray found no static surface
        float plate_top = 0.0f;
        float max_collision_gap = 0.0f;       // max |terrain collision y - plate top| just outside
        float max_render_gap = 0.0f;          // the same for the RENDERED (drawn) terrain height
        float max_render_vs_collision = 0.0f; // |rendered - collision| at the same point
        float worst_x = 0.0f;
        float worst_z = 0.0f;
        std::array<bool, 4> walked{};         // N, E, S, W: reached 5 m inside, no jump
        std::array<float, 4> walk_progress{}; // metres of the 25 m approach actually covered
        bool ok = false;
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

    [[nodiscard]] const PlateRect& plate() const noexcept { return plate_; }

    // The owner could not walk from the outer terrain onto the city plate. This proves the two
    // walkable surfaces are continuous along the WHOLE plate perimeter, on the server's real
    // physics world and real terrain collision: (1) at >= 64 points just outside the edge, the
    // terrain's collision height and its drawn height are each within the character's step-up
    // height of the plate top; (2) the real `step_character`, never jumping, walks from 20 m
    // outside to 5 m inside from all four sides. `self` is the avatar's own body (excluded from
    // casts); `tick` must exceed every tick already given to `server().collision.plan`.
    // Prints a per-point table to `report` when non-null.
    [[nodiscard]] PlateProof prove_plate_boundary(const gameplay::CharacterConfig& character,
                                                  physics::BodyId self,
                                                  std::uint64_t tick,
                                                  std::FILE* report);

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

    [[nodiscard]] static assets::ProceduralTerrainConfig
    street_config(ecs::World& world, bool& valid, PlateRect* plate = nullptr);
    [[nodiscard]] static physics::Aabb street_bounds(const assets::ProceduralTerrainConfig& cfg);
    [[nodiscard]] std::vector<Key> wanted(core::Vec3 eye) const;
    [[nodiscard]] Held* find(const Key& key) noexcept;

    bool valid_ = false;
    PlateRect plate_{}; // declared BEFORE config_: street_config fills it during config_'s init
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
