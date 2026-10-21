// SPDX-License-Identifier: Apache-2.0
#include "play_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "play_environment.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/gameplay/character.hpp"
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

assets::ProceduralTerrainConfig
PlayWorld::street_config(ecs::World& world, bool& valid, PlateRect* plate) {
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
        if (plate != nullptr) {
            *plate = {p.x - s.half_x, p.x + s.half_x, p.z - s.half_z, p.z + s.half_z, p.y};
        }
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
    : config_(street_config(server_world, valid_, &plate_)),
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

// ── The plate-boundary proof ─────────────────────────────────────────────────────────────────
//
// WHY A CONTINUITY PROOF AND NOT A "CAN STEP UP" TEST. The failure the owner met is a property of
// two surfaces that were authored apart: the city plate's walkable top (a physics slab, y = 0) and
// the streamed terrain beside it (a heightfield that dips to -1.5 m at the street rectangle). A
// character's step-up ladder climbs 0.3 m; its jump apex is v^2 / 2g = 1.03 m. A 1.5 m lip is
// therefore a wall, however well the controller works. So the proof reads BOTH surfaces at many
// points on the real physics world and compares them, and then walks the real controller across
// as a second, independent witness.
namespace {

// Height of the drawn heightfield at world (x, z): the SAME asset the renderer uploads, sampled the
// way its triangles are laid out. Each cell is split along the diagonal from sample (i, j) to
// (i+1, j+1) (heightfield_asset.hpp), so the height is linear on whichever triangle holds the
// point. Reading the asset rather than the collision world is what makes "rendered == collision" a
// measurement instead of an assumption: they share a generator, and this checks that they agree.
bool rendered_height(const assets::ProceduralTerrainConfig& config, float x, float z, float& out) {
    const std::int64_t width = assets::procedural_tile_width_metres(0);
    const std::int32_t tx = tile_at(x, width);
    const std::int32_t tz = tile_at(z, width);
    const auto asset = assets::generate_procedural_tile(config, tx, tz, 0);
    if (!asset) {
        return false;
    }
    const float lx = (x - asset->origin.x) / asset->cell_size_x;
    const float lz = (z - asset->origin.z) / asset->cell_size_z;
    const auto i = static_cast<std::uint32_t>(
        std::clamp(std::floor(lx), 0.0f, static_cast<float>(asset->columns - 2)));
    const auto j = static_cast<std::uint32_t>(
        std::clamp(std::floor(lz), 0.0f, static_cast<float>(asset->rows - 2)));
    const float fx = lx - static_cast<float>(i);
    const float fz = lz - static_cast<float>(j);
    const float h00 = asset->height(i, j);
    const float h10 = asset->height(i + 1, j);
    const float h01 = asset->height(i, j + 1);
    const float h11 = asset->height(i + 1, j + 1);
    out = fx >= fz ? h00 + (h10 - h00) * fx + (h11 - h10) * fz
                   : h00 + (h11 - h01) * fx + (h01 - h00) * fz;
    return true;
}

} // namespace

PlayWorld::PlateProof PlayWorld::prove_plate_boundary(const gameplay::CharacterConfig& character,
                                                      physics::BodyId self,
                                                      std::uint64_t tick,
                                                      std::FILE* report) {
    PlateProof r;
    r.plate_top = plate_.top_y;
    physics::PhysicsWorld& world = server_.physics;
    physics::QueryFilter statics;
    statics.dynamics = false;

    // Make the collision tiles under `centre` resident, the way a ticking game does: a demand for
    // the player's neighbourhood, committed at the barrier. A sample with no tile is COUNTED, never
    // skipped silently -- a proof that cannot see what it skipped still reads as passing.
    auto ensure = [&](core::Vec3 centre, float radius) {
        const std::vector<terrain_collision::Demand> demands{
            {around(centre, radius), terrain_collision::DemandKind::Body}};
        const auto plan = server_.collision.plan(++tick, demands);
        auto status = server_.collision.try_commit(plan);
        if (status == terrain_collision::CommitStatus::Waiting) {
            status = server_.collision.try_commit(plan);
        }
        server_.collision.clear_journal();
        return status == terrain_collision::CommitStatus::Ready;
    };
    auto height_at = [&](float x, float z, float& y) {
        physics::RayHit hit;
        const float top = plate_.top_y + 60.0f;
        if (!world.raycast(
                physics::Ray{{x, top, z}, {0.0f, -1.0f, 0.0f}, top + 64.0f}, hit, statics)) {
            return false;
        }
        y = hit.point.y;
        return true;
    };

    // ── (1) the perimeter: 16 points a side, 0.25 m inside and 0.25 m outside the edge ──────────
    constexpr int kPerSide = 16;
    constexpr float kProbe = 0.25f;
    const float step = character.step_height;
    if (report) {
        std::fprintf(report,
                     "side  along     plate_top  coll_out  render_out  gap_coll  gap_render\n");
    }

    // Sides in compass order N (-Z), E (+X), S (+Z), W (-X); `inward` is the unit step onto the
    // plate.
    struct Side {
        char name;
        float ox, oz; // outward direction
    };

    const std::array<Side, 4> sides{{{'N', 0, -1}, {'E', 1, 0}, {'S', 0, 1}, {'W', -1, 0}}};
    auto edge_point = [&](const Side& sd, float t, float out_by, float& x, float& z) {
        // t in (0,1) runs along the edge; out_by is metres outward (negative = onto the plate).
        if (sd.oz != 0.0f) {
            x = plate_.min_x + t * (plate_.max_x - plate_.min_x);
            z = (sd.oz < 0 ? plate_.min_z : plate_.max_z) + sd.oz * out_by;
        } else {
            z = plate_.min_z + t * (plate_.max_z - plate_.min_z);
            x = (sd.ox < 0 ? plate_.min_x : plate_.max_x) + sd.ox * out_by;
        }
    };
    for (const Side& sd : sides) {
        for (int k = 0; k < kPerSide; ++k) {
            const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(kPerSide);
            float ix, iz, ox, oz;
            edge_point(sd, t, -kProbe, ix, iz);
            edge_point(sd, t, kProbe, ox, oz);
            if (!ensure({ox, plate_.top_y, oz}, 4.0f) || !ensure({ix, plate_.top_y, iz}, 4.0f)) {
                ++r.uncovered;
                continue;
            }
            float plate_y, coll_y, rend_y;
            if (!height_at(ix, iz, plate_y) || !height_at(ox, oz, coll_y) ||
                !rendered_height(config_, ox, oz, rend_y)) {
                ++r.no_hit;
                continue;
            }
            ++r.samples;
            const float gap_c = std::abs(coll_y - plate_y);
            const float gap_r = std::abs(rend_y - plate_y);
            r.max_render_vs_collision =
                std::max(r.max_render_vs_collision, std::abs(rend_y - coll_y));
            r.max_render_gap = std::max(r.max_render_gap, gap_r);
            if (gap_c >= r.max_collision_gap) {
                r.max_collision_gap = gap_c;
                r.worst_x = ox;
                r.worst_z = oz;
            }
            if (report) {
                std::fprintf(report,
                             "%c   %8.2f  %9.3f  %8.3f  %10.3f  %8.3f  %10.3f\n",
                             sd.name,
                             sd.oz != 0.0f ? ox : oz,
                             plate_y,
                             coll_y,
                             rend_y,
                             gap_c,
                             gap_r);
            }
        }
    }

    // A profile outward from each side's centre: where the terrain sits at 0.25 .. 16 m out.
    if (report) {
        std::fprintf(report, "outward profile at each side's centre (collision y, rendered y):\n");
        for (const Side& sd : sides) {
            std::fprintf(report, "  %c:", sd.name);
            for (const float d : {0.25f, 1.0f, 2.0f, 4.0f, 8.0f, 12.0f, 16.0f}) {
                float x, z, cy = 0, ry = 0;
                edge_point(sd, 0.5f, d, x, z);
                (void)ensure({x, plate_.top_y, z}, 4.0f);
                const bool a = height_at(x, z, cy);
                const bool b = rendered_height(config_, x, z, ry);
                std::fprintf(report, "  %.2fm: %.3f/%.3f", d, a ? cy : NAN, b ? ry : NAN);
            }
            std::fprintf(report, "\n");
        }
    }

    // ── (2) the scripted walk: the real controller, forward only, never the jump bit ────────────
    // From 20 m outside to 5 m inside, along a line perpendicular to the edge. A building can stand
    // on the plate where the line ends, and then "blocked" would be the building's doing, not the
    // terrain's, so each side tries several lateral positions and passes if ANY one gets through.
    // The sensitivity control is the same call on the unmodified terrain: every line fails there.
    constexpr float kLateral[] = {0.5f, 0.35f, 0.65f, 0.2f, 0.8f, 0.1f, 0.9f};
    const float half_total = character.half_height + character.radius;
    for (std::size_t si = 0; si < sides.size(); ++si) {
        const Side& sd = sides[si];
        // yaw: engine forward is -Z at yaw 0 and -X at +pi/2, so forward = (-sin yaw, -cos yaw).
        const float inward_x = -sd.ox, inward_z = -sd.oz;
        const float yaw = std::atan2(-inward_x, -inward_z);
        for (const float t : kLateral) {
            float sx, sz;
            edge_point(sd, t, 20.0f, sx, sz);
            if (!ensure({sx, plate_.top_y, sz}, 6.0f)) {
                continue;
            }
            float ground = 0.0f;
            if (!height_at(sx, sz, ground)) {
                continue;
            }
            gameplay::CharacterState state;
            state.position = {sx, ground + half_total + character.skin + 0.05f, sz};
            gameplay::CharacterInput settle{};
            settle.yaw = yaw;
            for (int i = 0; i < 30; ++i) { // land on the terrain before asking it to walk
                (void)ensure(state.position, 3.0f);
                state = gameplay::step_character(state, settle, character, world, self, kDt);
            }
            gameplay::CharacterInput walk = settle;
            walk.move_y = 1.0f;
            float along = 0.0f;
            bool crossed = false;
            for (int i = 0; i < 600 && !crossed; ++i) {
                (void)ensure(state.position, 3.0f + character.max_speed * kDt);
                state = gameplay::step_character(state, walk, character, world, self, kDt);
                along = (state.position.x - sx) * inward_x + (state.position.z - sz) * inward_z;
                // 20 m outside + 5 m inside = 25 m of travel; and the feet must be ON the plate
                // (y near its top), not tunnelled under it or dropped into a trench.
                crossed = along >= 25.0f &&
                          std::abs(state.position.y - half_total - plate_.top_y) < 0.25f;
                if (!std::isfinite(state.position.y) || state.position.y < plate_.top_y - 6.0f) {
                    break;
                }
            }
            r.walk_progress[si] = std::max(r.walk_progress[si], along);
            if (crossed) {
                r.walked[si] = true;
                break;
            }
        }
    }

    r.ok = r.uncovered == 0 && r.no_hit == 0 && r.samples >= 64 && r.max_collision_gap <= step &&
           r.max_render_gap <= step && r.max_render_vs_collision <= 0.02f && r.walked[0] &&
           r.walked[1] && r.walked[2] && r.walked[3];
    return r;
}

} // namespace rime::block_play
