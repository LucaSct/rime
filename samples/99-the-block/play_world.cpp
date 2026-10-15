// SPDX-License-Identifier: Apache-2.0
#include "play_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "play_environment.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ground/surface.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/components.hpp"
#include "rime/render/terrain_lod.hpp"

namespace rime::block_play {
namespace {

constexpr float kDt = 1.0f / 60.0f;

bool finite(core::Vec3 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

physics::Aabb around(core::Vec3 p, float radius) noexcept {
    const core::Vec3 r{radius, radius, radius};
    return {p - r, p + r};
}

std::int32_t tile_at(float metres, std::int64_t width) noexcept {
    return static_cast<std::int32_t>(std::floor(static_cast<double>(metres) / width));
}

std::int32_t even_center(float metres, std::int64_t width) noexcept {
    return 2 *
           static_cast<std::int32_t>(std::floor(static_cast<double>(tile_at(metres, width)) / 2.0));
}

struct Square {
    std::int32_t min_x;
    std::int32_t min_z;
    std::int32_t max_x; // exclusive
    std::int32_t max_z;
};

Square square(core::Vec3 eye, std::uint32_t lod) noexcept {
    const auto width = assets::procedural_tile_width_metres(lod);
    const auto x = even_center(eye.x, width);
    const auto z = even_center(eye.z, width);
    return {x - 4, z - 4, x + 4, z + 4};
}

bool inside_finer(std::int32_t x, std::int32_t z, std::uint32_t lod, const Square& finer) noexcept {
    if (lod == 0) {
        return false;
    }
    return x * 2 >= finer.min_x && (x + 1) * 2 <= finer.max_x && z * 2 >= finer.min_z &&
           (z + 1) * 2 <= finer.max_z;
}

} // namespace

bool needs_recovery(core::Vec3 p) noexcept {
    return !finite(p) || std::abs(p.x) > PlayWorld::kPracticalLimit ||
           std::abs(p.z) > PlayWorld::kPracticalLimit || p.y < PlayWorld::kRecoveryFloor;
}

assets::ProceduralTerrainConfig PlayWorld::street_config(ecs::World& world, bool& valid) {
    assets::ProceduralTerrainConfig result{};
    valid = false;
    std::size_t count = 0;
    world.query<ground::GroundSurface>().for_each([&](ecs::Entity e, ground::GroundSurface& s) {
        const auto* t = world.get<ecs::WorldTransform>(e);
        if (!t || !finite(t->value.translation) || !std::isfinite(s.half_x) ||
            !std::isfinite(s.half_z) || s.half_x <= 0.0f || s.half_z <= 0.0f ||
            std::abs(t->value.translation.y) > 0.001f ||
            std::abs(t->value.translation.x) + s.half_x > kPracticalLimit ||
            std::abs(t->value.translation.z) + s.half_z > kPracticalLimit) {
            return;
        }
        const auto q = t->value.rotation;
        // Heightfield tiles are axis-aligned. A rotated street needs an explicitly transformed
        // footprint, not a silently incorrect flat region.
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) ||
            !std::isfinite(q.w) || std::abs(q.x) > 0.001f || std::abs(q.y) > 0.001f ||
            std::abs(q.z) > 0.001f || std::abs(q.w - 1.0f) > 0.001f) {
            return;
        }
        const auto p = t->value.translation;
        result.street_min_x = static_cast<std::int32_t>(std::floor(p.x - s.half_x));
        result.street_max_x = static_cast<std::int32_t>(std::ceil(p.x + s.half_x));
        result.street_min_z = static_cast<std::int32_t>(std::floor(p.z - s.half_z));
        result.street_max_z = static_cast<std::int32_t>(std::ceil(p.z + s.half_z));
        ++count;
    });
    valid = count == 1 && assets::procedural_config_valid(result);
    return result;
}

physics::Aabb PlayWorld::street_bounds(const assets::ProceduralTerrainConfig& c) {
    return {{static_cast<float>(c.street_min_x) - 64.0f,
             -8.0f,
             static_cast<float>(c.street_min_z) - 64.0f},
            {static_cast<float>(c.street_max_x) + 64.0f,
             8.0f,
             static_cast<float>(c.street_max_z) + 64.0f}};
}

PlayWorld::CollisionPeer::CollisionPeer(const assets::ProceduralTerrainConfig& config,
                                        const physics::Aabb& block,
                                        physics::PhysicsWorld& physics)
    : physics(physics), source(terrain_collision::ProceduralTileSource::Options{config, 256}),
      collision(
          [&] {
              terrain_collision::Config c{};
              c.origin = {0.0f, 0.0f, 0.0f};
              c.tile_size_x = 64.0f;
              c.tile_size_z = 64.0f;
              c.activate_margin_tiles = 2.0f;
              c.retain_margin_tiles = 3.0f;
              c.activation_lead_ticks = 0; // source is synchronous and always Ready
              c.max_required_tiles = 256;
              return c;
          }(),
          source,
          physics) {
    block_pin = collision.pin(block, config.version, terrain_collision::PinReason::Query);
}

bool PlayWorld::CollisionPeer::prepare(ecs::World& world,
                                       destruction::DestructionWorld& destruction,
                                       std::uint64_t tick,
                                       core::Vec3 player,
                                       float max_speed) {
    if (!block_pin.is_valid() || needs_recovery(player)) {
        ++failures;
        return false;
    }
    std::vector<terrain_collision::Demand> demands;
    demands.reserve(destruction.instance_count() + destruction.debris_count() + 64);
    bool states_valid = true;
    auto add_body = [&](physics::BodyId id) {
        physics::BodyState s;
        if (!id.is_valid() || !physics.is_alive(id)) {
            return;
        }
        if (!physics.get_body_state(id, s) || !finite(s.position) || !finite(s.linear_velocity) ||
            std::abs(s.position.x) > kPracticalLimit || std::abs(s.position.z) > kPracticalLimit) {
            states_valid = false;
            return;
        }
        physics::Aabb bounds;
        if (!physics.broadphase_aabb(id, bounds)) {
            states_valid = false;
            return;
        }
        // Use the shape's current broadphase bounds, including rotation and sleeping bodies.
        // A fixed radius would silently stop covering a larger authored piece. The sweep adds
        // next-tick velocity; the margin covers controller and solver correction.
        demands.push_back({terrain_collision::swept_bounds(bounds, s.linear_velocity, kDt, 2.0f),
                           terrain_collision::DemandKind::Body});
    };
    world.query<physics::RigidBodyHandle>().for_each(
        [&](ecs::Entity, physics::RigidBodyHandle& h) { add_body(h.body); });
    for (std::size_t i = 0; i < destruction.instance_count(); ++i) {
        add_body(destruction.body_of(destruction::InstanceId{static_cast<std::uint32_t>(i), 0}));
    }
    for (std::size_t i = 0; i < destruction.debris_count(); ++i) {
        add_body(destruction.debris_body(i));
    }
    if (!states_valid) {
        ++failures;
        return false;
    }
    const float player_margin = 2.0f + max_speed * kDt;
    const auto player_bounds = around(player, player_margin);
    demands.push_back({player_bounds, terrain_collision::DemandKind::Body});
    const auto plan = collision.plan(tick, demands);
    auto status = collision.try_commit(plan);
    // A synchronous source can need a second barrier pass after its first request. Never advance
    // gameplay while it reports Waiting, and never replace the plan during the retry.
    if (status == terrain_collision::CommitStatus::Waiting) {
        status = collision.try_commit(plan);
    }
    collision.clear_journal();
    if (status != terrain_collision::CommitStatus::Ready || !collision.covers(player_bounds)) {
        ++failures;
        return false;
    }
    ++barriers;
    return true;
}

PlayWorld::PlayWorld(ecs::World& server_world,
                     physics::PhysicsWorld& server_physics,
                     physics::PhysicsWorld& client_physics,
                     rhi::Device* device)
    : config_(street_config(server_world, valid_)),
      server_(config_, street_bounds(config_), server_physics),
      client_(config_, street_bounds(config_), client_physics), device_(device) {
    valid_ = valid_ && server_.block_pin.is_valid() && client_.block_pin.is_valid();
    held_.reserve(304);
    draws_.reserve(304);
    slot_tiles_.fill(render::kInvalidTerrainTile);
    if (device_) {
        pass_ = std::make_unique<render::TerrainPass>(*device_);
    }
}

PlayWorld::~PlayWorld() {
    if (device_ && pass_) {
        device_->wait_idle();
        for (const Held& h : held_) {
            (void)pass_->release(h.tile);
        }
    }
}

bool PlayWorld::prepare(ecs::World& server_world,
                        destruction::DestructionWorld& server_destruction,
                        ecs::World& client_world,
                        destruction::DestructionWorld& client_destruction,
                        std::uint64_t tick,
                        core::Vec3 server_player,
                        core::Vec3 client_player,
                        float max_speed) {
    if (!valid_) {
        return false;
    }
    return server_.prepare(server_world, server_destruction, tick, server_player, max_speed) &&
           client_.prepare(client_world, client_destruction, tick, client_player, max_speed);
}

std::vector<PlayWorld::Key> PlayWorld::wanted(core::Vec3 eye) const {
    std::vector<Key> result;
    result.reserve(304);
    for (std::int32_t level = 5; level >= 0; --level) {
        const auto box = square(eye, static_cast<std::uint32_t>(level));
        const auto finer = level > 0 ? square(eye, static_cast<std::uint32_t>(level - 1)) : box;
        for (std::int32_t z = box.min_z; z < box.max_z; ++z) {
            for (std::int32_t x = box.min_x; x < box.max_x; ++x) {
                if (!inside_finer(x, z, static_cast<std::uint32_t>(level), finer)) {
                    result.push_back({x, z, static_cast<std::uint32_t>(level)});
                }
            }
        }
    }
    return result;
}

PlayWorld::Held* PlayWorld::find(const Key& key) noexcept {
    const auto it =
        std::find_if(held_.begin(), held_.end(), [&](const Held& h) { return h.key == key; });
    return it == held_.end() ? nullptr : &*it;
}

bool PlayWorld::recenter(core::Vec3 eye) {
    if (!pass_) {
        return true;
    }
    if (needs_recovery(eye)) {
        ++stats_.refusals;
        return false;
    }
    const std::vector<Key> want = wanted(eye);
    if (want.size() > 304) {
        ++stats_.refusals;
        return false;
    }
    const bool changed =
        want.size() != held_.size() ||
        std::any_of(want.begin(), want.end(), [&](const Key& key) { return find(key) == nullptr; });
    if (changed && !held_.empty()) {
        // Last graph has already been submitted when this callback runs again. Retire all of its
        // readers before ANY resource can be released or a slot recycled. This wait is recenter
        // only; unchanged ordinary frames never call it.
        device_->wait_idle();
        ++stats_.recenters;
        for (auto it = held_.begin(); it != held_.end();) {
            if (std::find(want.begin(), want.end(), it->key) != want.end()) {
                ++it;
                continue;
            }
            (void)slots_.evict(it->slot);
            stats_.gpu_bytes -= it->bytes;
            --stats_.resident;
            ++stats_.evictions;
            it = held_.erase(it);
        }
        std::vector<std::uint32_t> reclaimed;
        (void)slots_.reclaim(frame_, reclaimed); // wait_idle confirms this watermark
        for (const auto slot : reclaimed) {
            (void)pass_->release(slot_tiles_[slot]);
            slot_tiles_[slot] = render::kInvalidTerrainTile;
        }
    }
    for (const Key& key : want) {
        if (find(key)) {
            continue;
        }
        auto asset = assets::generate_procedural_tile(config_, key.x, key.z, key.lod);
        if (!asset) {
            ++stats_.refusals;
            return false;
        }
        if (dressed_surface_) {
            PlayEnvironment::paint_terrain(*asset, config_);
        }
        const auto bytes = render::TerrainPass::predicted_tile_bytes(*asset, false);
        if (stats_.resident >= kMaxGpuSlots || stats_.gpu_bytes + bytes > kMaxGpuBytes) {
            ++stats_.refusals;
            return false;
        }
        const auto slot = slots_.acquire();
        if (!slot) {
            ++stats_.refusals;
            return false;
        }
        const auto tile =
            dressed_surface_ ? pass_->upload(*asset, surface_palette_) : pass_->upload(*asset);
        if (tile == render::kInvalidTerrainTile) {
            (void)slots_.abandon(*slot);
            ++stats_.refusals;
            return false;
        }
        slot_tiles_[slot->slot] = tile;
        held_.push_back({key, *slot, tile, bytes});
        ++stats_.resident;
        ++stats_.uploads;
        stats_.gpu_bytes += bytes;
        stats_.peak_resident = std::max(stats_.peak_resident, stats_.resident);
        stats_.peak_gpu_bytes = std::max(stats_.peak_gpu_bytes, stats_.gpu_bytes);
    }
    draws_.clear();
    for (const Key& key : want) {
        Held* h = find(key);
        if (!h) {
            ++stats_.refusals;
            return false;
        }
        render::TerrainLodDraw lod{};
        lod.enabled = true;
        lod.level = key.lod;
        lod.base_x = key.x * static_cast<std::int32_t>(assets::kProceduralTileCells);
        lod.base_z = key.z * static_cast<std::int32_t>(assets::kProceduralTileCells);
        lod.camera = eye;
        lod.morph_start = std::numeric_limits<float>::infinity();
        lod.morph_end = std::numeric_limits<float>::infinity();
        if (key.lod < 5) {
            const auto box = square(eye, key.lod);
            if (key.x == box.min_x)
                lod.coarser_edges |= render::kTerrainEdgeNegX;
            if (key.x == box.max_x - 1)
                lod.coarser_edges |= render::kTerrainEdgePosX;
            if (key.z == box.min_z)
                lod.coarser_edges |= render::kTerrainEdgeNegZ;
            if (key.z == box.max_z - 1)
                lod.coarser_edges |= render::kTerrainEdgePosZ;
        }
        if (key.lod < 5) {
            const Key parent{static_cast<std::int32_t>(std::floor(key.x / 2.0)),
                             static_cast<std::int32_t>(std::floor(key.z / 2.0)),
                             key.lod + 1};
            if (const Held* p = find(parent)) {
                lod.parent = p->tile;
                lod.parent_quadrant = static_cast<std::uint32_t>(key.x & 1) |
                                      (static_cast<std::uint32_t>(key.z & 1) << 1);
            }
        }
        draws_.push_back({h->tile, lod});
    }
    return true;
}

void PlayWorld::mark_drawn() noexcept {
    ++frame_;
    for (const Held& h : held_) {
        (void)slots_.mark_read(h.slot, frame_);
    }
}

} // namespace rime::block_play
