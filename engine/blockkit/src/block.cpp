// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/blockkit/block.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include "rime/blockkit/palette.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"

namespace rime::blockkit {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// A four-character content id, the convention 12-networked-destruction established.
constexpr std::uint64_t fourcc(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint64_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint64_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint64_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint64_t>(static_cast<unsigned char>(d));
}

// The nine cooks. Dimensions describe the DEFAULT BlockParams — 8 m footprint, 3 m storeys, 0.3 m
// slabs, a 2 m doorway. A caller that changes those dimension fields is asking for geometry no
// cook matches; the count fields (buildings, storeys, crates, lamps) are free to vary, which is
// what the tests exercise.
//
// Background at 12 parts a slab and hero at 28 is a deliberate ~2.3x: ADR-0035 §1 wants >= 400 peak
// live debris, and one background building's 180 parts cannot reach it even if every part detached.
constexpr std::array<CookSpec, 9> kCooks{{
    // Front and back walls: the full footprint.
    {"wall_bg", 8.0f, 3.0f, 0.3f, 12, 1301, fourcc('B', 'G', 'W', 'L')},
    {"wall_hero", 8.0f, 3.0f, 0.3f, 28, 1302, fourcc('H', 'R', 'W', 'L')},
    // Side walls: shortened by one slab thickness at each end so they butt between the front and
    // back walls instead of overlapping them at the corners (see block.hpp's geometry note).
    {"side_bg", 7.4f, 3.0f, 0.3f, 12, 1303, fourcc('B', 'G', 'S', 'W')},
    {"side_hero", 7.4f, 3.0f, 0.3f, 28, 1304, fourcc('H', 'R', 'S', 'W')},
    // Horizontal slabs (interior floors and the roof): inset the same way, on both axes.
    {"floor_bg", 7.4f, 0.3f, 7.4f, 12, 1305, fourcc('B', 'G', 'F', 'L')},
    {"floor_hero", 7.4f, 0.3f, 7.4f, 28, 1306, fourcc('H', 'R', 'F', 'L')},
    // Each half of the split front ground wall; the gap between them is the doorway.
    {"half_bg", 3.0f, 3.0f, 0.3f, 6, 1307, fourcc('B', 'G', 'H', 'F')},
    {"half_hero", 3.0f, 3.0f, 0.3f, 14, 1308, fourcc('H', 'R', 'H', 'F')},
    // Street crates — a second destructible class for the collapse to interact with.
    {"crate", 1.0f, 1.0f, 1.0f, 8, 1309, fourcc('C', 'R', 'A', 'T')},
}};

// Indices into kCooks, paired background/hero so the prefab picks with one branch.
enum CookSlot : std::size_t {
    kWall = 0,
    kSide = 2,
    kFloorSlab = 4,
    kHalf = 6,
    kCrate = 8,
};

[[nodiscard]] const CookSpec& cook(CookSlot slot, bool hero) noexcept {
    return kCooks[static_cast<std::size_t>(slot) + (hero ? 1u : 0u)];
}

[[nodiscard]] core::Quat yaw(float radians) noexcept {
    return core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, radians);
}

// Is building `b` one of the two heroes? Buildings are numbered south side first.
[[nodiscard]] bool is_hero(std::uint32_t b, const BlockParams& p) noexcept {
    const std::uint32_t side = b / p.buildings_per_side;
    const std::uint32_t idx = b % p.buildings_per_side;
    return side == 0 ? idx == p.hero_south : idx == p.hero_north;
}

// Where a building sits, and which way it faces. The street runs +X; south buildings are at -Z and
// face +Z, north buildings at +Z and face -Z. Expressing that as a yaw rather than as two mirrored
// layouts is what keeps the slab loop below written once.
[[nodiscard]] core::Transform building_frame(std::uint32_t b, const BlockParams& p) noexcept {
    const std::uint32_t side = b / p.buildings_per_side;
    const std::uint32_t idx = b % p.buildings_per_side;

    core::Transform tf;
    tf.translation.x =
        p.footprint * 0.5f + static_cast<float>(idx) * (p.footprint + p.building_gap);
    tf.translation.y = 0.0f;
    tf.translation.z = (p.street_width + p.footprint) * 0.5f * (side == 0 ? -1.0f : 1.0f);
    tf.rotation = yaw(side == 0 ? 0.0f : kPi);
    return tf;
}

// A deterministic scatter for the crates: a 32-bit integer hash, so the same params always produce
// the same street and the generated `.rscene` is byte-reproducible. Deliberately not a PRNG object
// — no hidden sequence state means the i-th crate does not move when the count changes.
[[nodiscard]] float hash01(std::uint32_t x, std::uint32_t salt) noexcept {
    std::uint32_t h = x * 0x9E37'79B9u + salt * 0x85EB'CA6Bu;
    h ^= h >> 15;
    h *= 0x2C1B'3C6Du;
    h ^= h >> 12;
    h *= 0x297A'2D39u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFF'FFFFu) / static_cast<float>(0x100'0000u);
}

} // namespace

std::span<const CookSpec> cook_specs() noexcept {
    return kCooks;
}

const CookSpec* find_cook(std::string_view name) noexcept {
    for (const CookSpec& spec : kCooks) {
        if (name == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

const CookSpec* find_cook_by_asset(std::uint64_t asset) noexcept {
    for (const CookSpec& spec : kCooks) {
        if (asset == spec.asset) {
            return &spec;
        }
    }
    return nullptr;
}

float BlockParams::street_length() const noexcept {
    if (buildings_per_side == 0) {
        return 0.0f;
    }
    return static_cast<float>(buildings_per_side) * footprint +
           static_cast<float>(buildings_per_side - 1) * building_gap;
}

std::uint32_t BlockParams::slabs_per_building() const noexcept {
    // Four walls a storey, plus one extra for the split front ground wall, plus one horizontal slab
    // a storey (interior floors, and the roof on top of the last).
    return storeys * 5u + 1u;
}

core::Vec3 west_viewpoint(const BlockParams& p) noexcept {
    return {-p.building_gap - 2.0f, kEyeHeight, 0.0f};
}

core::Vec3 east_viewpoint(const BlockParams& p) noexcept {
    return {p.street_length() + p.building_gap + 2.0f, kEyeHeight, 0.0f};
}

std::size_t derive_world_transforms(ecs::World& world) {
    (void)world.register_component<ecs::WorldTransform>();

    // Collect then write: adding a component relocates the entity between archetypes, which the
    // query contract forbids during iteration.
    std::vector<std::pair<ecs::Entity, core::Transform>> pending;
    world.query<ecs::LocalTransform>().for_each(
        [&](ecs::Entity e, ecs::LocalTransform& tf) { pending.emplace_back(e, tf.value); });
    for (const auto& [entity, tf] : pending) {
        (void)world.add_component(entity, ecs::WorldTransform{tf});
    }
    return pending.size();
}

BlockStats predict(const BlockParams& p) noexcept {
    BlockStats s;
    s.buildings = p.building_count();
    s.slabs = s.buildings * p.slabs_per_building();
    s.crates = p.crates;

    const std::size_t lamps = static_cast<std::size_t>(p.lamps_per_side) * 2u;
    s.props = 1u                         // the street
              + 2u                       // two kerbs
              + p.barriers + lamps * 2u; // each lamp is a mast and an emissive head

    s.point_lights = s.buildings * p.storeys; // one per storey per building
    s.spot_lights = lamps;
    s.dir_lights = 1u;

    // Parts, summed from the cooks each slab actually names.
    const std::size_t heroes = (p.buildings_per_side > p.hero_south ? 1u : 0u) +
                               (p.buildings_per_side > p.hero_north ? 1u : 0u);
    const std::size_t background = s.buildings - heroes;
    for (std::size_t i = 0; i < 2; ++i) {
        const bool hero = i == 1;
        const std::size_t count = hero ? heroes : background;
        const std::size_t per_building =
            static_cast<std::size_t>(p.storeys - 1) * cook(kWall, hero).parts // front, above ground
            + static_cast<std::size_t>(p.storeys) * cook(kWall, hero).parts   // back
            + static_cast<std::size_t>(p.storeys) * 2u * cook(kSide, hero).parts +
            2u * cook(kHalf, hero).parts +
            static_cast<std::size_t>(p.storeys) * cook(kFloorSlab, hero).parts;
        s.parts += count * per_building;
    }
    s.parts += static_cast<std::size_t>(p.crates) * cook(kCrate, false).parts;

    s.entities = s.slabs + s.crates + s.props + s.point_lights + s.spot_lights + s.dir_lights +
                 1u; // the camera
    return s;
}

BlockStats assemble(ecs::World& world, const BlockParams& p) {
    using ecs::LocalTransform;

    ecs::register_transform_components(world);
    register_blockkit_components(world);
    destruction::register_destruction_components(world);
    render::register_render_components(world);

    BlockStats stats;
    stats.buildings = p.building_count();

    const auto spawn_destructible =
        [&](const core::Transform& tf, const SlabRole& role, const CookSpec& spec) {
            (void)world.spawn_with(LocalTransform{tf}, role, destruction::Destructible{spec.asset});
            ++stats.entities;
            stats.parts += spec.parts;
        };
    const auto spawn_prop = [&](const core::Transform& tf, const SlabRole& role) {
        (void)world.spawn_with(LocalTransform{tf}, role);
        ++stats.entities;
        ++stats.props;
    };

    const float half_f = p.footprint * 0.5f;
    const float inset = half_f - p.slab_thickness * 0.5f; // wall centre offset from building centre
    const float half_wall_width = (p.footprint - p.doorway_width) * 0.5f;

    // ── The buildings ────────────────────────────────────────────────────────────────────────────
    for (std::uint32_t b = 0; b < stats.buildings; ++b) {
        const core::Transform frame = building_frame(b, p);
        const bool hero = is_hero(b, p);
        const std::uint32_t side = b / p.buildings_per_side;
        const std::uint32_t idx = b % p.buildings_per_side;
        const std::uint32_t tint = static_cast<std::uint32_t>((idx + 2u * side) % kTintCount);

        const auto slab = [&](core::Transform local,
                              std::uint32_t storey,
                              std::uint32_t kind,
                              CookSlot slot) {
            spawn_destructible(frame * local, SlabRole{b, storey, kind, tint}, cook(slot, hero));
            ++stats.slabs;
        };

        for (std::uint32_t s = 0; s < p.storeys; ++s) {
            const float wall_y = static_cast<float>(s) * p.storey_height + p.storey_height * 0.5f;

            // Front wall (facing the street). On the ground storey it is split in two, and the gap
            // between the halves IS the doorway — `rime fracture` only makes boxes, so an opening
            // can only be authored as space left between slabs.
            if (s == 0) {
                for (const float sign : {-1.0f, 1.0f}) {
                    core::Transform t;
                    t.translation = {sign * (half_f - half_wall_width * 0.5f), wall_y, inset};
                    slab(t, s, slab_kind::kHalfWall, kHalf);
                }
            } else {
                core::Transform t;
                t.translation = {0.0f, wall_y, inset};
                slab(t, s, slab_kind::kWall, kWall);
            }

            // Back wall.
            {
                core::Transform t;
                t.translation = {0.0f, wall_y, -inset};
                slab(t, s, slab_kind::kWall, kWall);
            }

            // Side walls: yawed a quarter turn so their length runs along Z.
            for (const float sign : {-1.0f, 1.0f}) {
                core::Transform t;
                t.translation = {sign * inset, wall_y, 0.0f};
                t.rotation = yaw(kPi * 0.5f);
                slab(t, s, slab_kind::kSideWall, kSide);
            }

            // The horizontal slab capping this storey: an interior floor, or the roof on the last.
            {
                const bool roof = s + 1u == p.storeys;
                core::Transform t;
                t.translation = {0.0f, static_cast<float>(s + 1) * p.storey_height, 0.0f};
                slab(t,
                     roof ? kRoofStorey : s + 1u,
                     roof ? slab_kind::kRoof : slab_kind::kFloor,
                     kFloorSlab);
            }
        }

        // One warm point light per storey, hung near the ceiling. Its radius (6 m) is smaller than
        // the 8 m footprint on purpose: the light stays in the room it belongs to, so breaching the
        // wall reveals a light that was always burning rather than switching one on.
        for (std::uint32_t s = 0; s < p.storeys; ++s) {
            core::Transform t = frame;
            t.translation.y = static_cast<float>(s + 1) * p.storey_height - kInteriorCeilingDrop;
            (void)world.spawn_with(LocalTransform{t},
                                   SlabRole{b, s, slab_kind::kLight, tint},
                                   render::PointLight{kInteriorColor[0],
                                                      kInteriorColor[1],
                                                      kInteriorColor[2],
                                                      kInteriorIntensity,
                                                      kInteriorRadius});
            ++stats.entities;
            ++stats.point_lights;
        }
    }

    // ── The street surface ───────────────────────────────────────────────────────────────────────
    const float length = p.street_length();
    {
        // A unit plane scaled out past the block on every side, so the ground never ends inside
        // the frame. Scaling here rather than baking a sized mesh keeps the street's dimensions
        // out of the palette (palette.hpp's unit-primitive note).
        const float span = length + 4.0f * p.footprint;
        core::Transform t;
        t.translation = {length * 0.5f, 0.0f, 0.0f};
        t.scale = {span, 1.0f, span};
        spawn_prop(t, SlabRole{kNoBuilding, 0, slab_kind::kStreet, 0});
    }

    // Kerbs: low strips just inside the building line, so rubble that reaches the road has an edge
    // to stack against rather than a seamless plane to slide across.
    for (const float sign : {-1.0f, 1.0f}) {
        core::Transform t;
        t.translation = {length * 0.5f, 0.075f, sign * (p.street_width * 0.5f - 0.2f)};
        t.scale = {length, 0.15f, 0.4f};
        spawn_prop(t, SlabRole{kNoBuilding, 0, slab_kind::kKerb, 0});
    }

    // Barriers, alternating sides down the street. Static, non-destructible, and load-bearing for
    // the look of a collapse: debris piles against them instead of spreading into a thin smear.
    for (std::uint32_t i = 0; i < p.barriers; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(p.barriers);
        core::Transform t;
        t.translation = {u * length, 0.5f, ((i % 2u) == 0u ? -1.0f : 1.0f) * 4.4f};
        t.scale = {2.0f, 1.0f, 0.5f};
        spawn_prop(t, SlabRole{kNoBuilding, 0, slab_kind::kBarrier, 0});
    }

    // ── Street lamps ─────────────────────────────────────────────────────────────────────────────
    // A mast, an emissive head so the fixture reads as the source, and a spot aimed straight down.
    // Spots rather than points because spots are M10's local shadow casters: a lamp that does not
    // throw the rubble's shadow across the road is not earning its place in this demo.
    const core::Quat aim_down = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -kPi * 0.5f);
    for (std::uint32_t side = 0; side < 2u; ++side) {
        const float z = (side == 0 ? -1.0f : 1.0f) * (p.street_width * 0.5f - 0.7f);
        for (std::uint32_t i = 0; i < p.lamps_per_side; ++i) {
            const float x =
                (static_cast<float>(i) + 0.5f) / static_cast<float>(p.lamps_per_side) * length;

            core::Transform mast;
            mast.translation = {x, kLampHeight * 0.5f, z};
            mast.scale = {0.15f, kLampHeight, 0.15f};
            spawn_prop(mast, SlabRole{kNoBuilding, 0, slab_kind::kLampMast, 0});

            core::Transform head;
            head.translation = {x, kLampHeight + 0.1f, z};
            head.scale = {0.5f, 0.2f, 0.5f};
            spawn_prop(head, SlabRole{kNoBuilding, 0, slab_kind::kLampHead, 0});

            core::Transform light;
            light.translation = {x, kLampHeight, z};
            light.rotation = aim_down;
            (void)world.spawn_with(LocalTransform{light},
                                   SlabRole{kNoBuilding, 0, slab_kind::kLight, 0},
                                   render::SpotLight{kLampColor[0],
                                                     kLampColor[1],
                                                     kLampColor[2],
                                                     kLampIntensity,
                                                     kLampRange,
                                                     kLampInnerAngle,
                                                     kLampOuterAngle});
            ++stats.entities;
            ++stats.spot_lights;
        }
    }

    // ── Crates ───────────────────────────────────────────────────────────────────────────────────
    // Destructible, and scattered without scale — a cooked destructible's hulls are baked, so its
    // placement may rotate and translate but must never scale, or the physics parts and the drawn
    // parts would disagree.
    for (std::uint32_t i = 0; i < p.crates; ++i) {
        core::Transform t;
        t.translation = {hash01(i, 7u) * length,
                         0.5f,
                         (hash01(i, 11u) * 2.0f - 1.0f) * (p.street_width * 0.5f - 1.0f)};
        t.rotation = yaw(hash01(i, 13u) * kPi * 2.0f);
        spawn_destructible(t, SlabRole{kNoBuilding, 0, slab_kind::kCrate, 0}, cook(kCrate, false));
        ++stats.crates;
    }

    // ── The sun, and the camera ──────────────────────────────────────────────────────────────────
    // Low and raking straight down the street: yaw so forward is +X, then pitch down by the
    // elevation. Long shadows along the axis the camera looks down is the whole reason this
    // arrangement was chosen over a plaza.
    {
        core::Transform t;
        t.rotation = core::normalize(
            yaw(-kPi * 0.5f) * core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -kSunElevation));
        (void)world.spawn_with(
            LocalTransform{t},
            SlabRole{kNoBuilding, 0, slab_kind::kLight, 0},
            render::DirectionalLight{kSunColor[0], kSunColor[1], kSunColor[2], kSunIntensity});
        ++stats.entities;
        ++stats.dir_lights;
    }
    {
        core::Transform t;
        t.translation = west_viewpoint(p);
        t.rotation = yaw(-kPi * 0.5f); // looking down the street, +X
        (void)world.spawn_with(
            LocalTransform{t}, SlabRole{kNoBuilding, 0, slab_kind::kLight, 0}, render::Camera{});
        ++stats.entities;
    }

    return stats;
}

} // namespace rime::blockkit
