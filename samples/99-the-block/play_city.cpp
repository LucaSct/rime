// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_city.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

#include "rime/blockkit/block.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/core/math.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ground/surface.hpp"
#include "rime/render/components.hpp"

namespace rime::block_play::city {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
using core::Vec3;

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Procedural convex parts
//
// A fracture part is one CONVEX hull stored COM-centred, with its centre kept separately
// (`DestructiblePart::com`); the compound the physics builds is those hulls at those centres. The
// cook tool derives all of that by Voronoi-splitting a box. A lamp is not a box, so we author the
// hulls directly -- but through the same data type, so the destruction, replication and render
// layers cannot tell a generated pattern from a cooked one.
//
// TECHNIQUE: LOFTING. Every part here is a "prism between two rings": a frustum for the pole, a
// skewed square tube for the arm, a rectangle-to-rectangle taper for a barrier. Two rings of n
// corners give n side quads and two n-gon caps, all planar as long as the rings are similar and
// parallel -- which they are by construction -- so the result is convex without a hull solver.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

struct Loft {
    std::vector<Vec3> v;
    std::vector<std::vector<std::uint32_t>> faces;
};

[[nodiscard]] Loft loft(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    const auto n = static_cast<std::uint32_t>(a.size());
    Loft l;
    l.v = a;
    l.v.insert(l.v.end(), b.begin(), b.end());
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t j = (i + 1) % n;
        l.faces.push_back({i, j, n + j, n + i});
    }
    std::vector<std::uint32_t> bottom(n), top(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        bottom[i] = i;
        top[i] = n + i;
    }
    l.faces.push_back(std::move(bottom));
    l.faces.push_back(std::move(top));
    return l;
}

// Turn a lofted solid into a `DestructiblePart`.
//
// Winding is NOT trusted from the loft. `mesh_from_part` takes a face normal from the first three
// indices, and the physics hull wants faces wound outward, so each face is tested against the
// direction from the solid's interior to the face and reversed if it points inward. For a convex
// solid that test is exact, and it removes a whole class of "one cap is inside out" mistakes that
// would otherwise only show up as a part that renders black from one side.
[[nodiscard]] assets::DestructiblePart make_part(Loft l) {
    Vec3 mean{0, 0, 0};
    for (const Vec3& p : l.v) {
        mean = mean + p;
    }
    mean = mean * (1.0f / static_cast<float>(l.v.size()));

    // Volume and centroid by tetrahedra fanned from an interior point. The signed volumes are all
    // positive once the winding is outward, and the centroid is the volume-weighted mean of the
    // tetrahedra centroids -- the exact COM for uniform density, which is what the physics
    // compound will recompute and expects the cook to have agreed with.
    double volume = 0.0;
    Vec3 weighted{0, 0, 0};
    for (auto& f : l.faces) {
        Vec3 c{0, 0, 0};
        for (const std::uint32_t i : f) {
            c = c + l.v[i];
        }
        c = c * (1.0f / static_cast<float>(f.size()));
        const Vec3 nrm = core::cross(l.v[f[1]] - l.v[f[0]], l.v[f[2]] - l.v[f[0]]);
        if (core::dot(nrm, c - mean) < 0.0f) {
            std::reverse(f.begin(), f.end());
        }
        for (std::size_t k = 1; k + 1 < f.size(); ++k) {
            const Vec3 a = l.v[f[0]] - mean, b = l.v[f[k]] - mean, d = l.v[f[k + 1]] - mean;
            const float vol = core::dot(a, core::cross(b, d)) / 6.0f;
            volume += static_cast<double>(vol);
            weighted =
                weighted + (mean * 4.0f + l.v[f[0]] + l.v[f[k]] + l.v[f[k + 1]]) * (vol * 0.25f);
        }
    }
    assets::DestructiblePart part;
    part.volume = static_cast<float>(volume);
    part.com = weighted * (1.0f / part.volume);

    part.aabb_min = part.aabb_max = l.v[0];
    for (const Vec3& p : l.v) {
        part.aabb_min = {std::min(part.aabb_min.x, p.x),
                         std::min(part.aabb_min.y, p.y),
                         std::min(part.aabb_min.z, p.z)};
        part.aabb_max = {std::max(part.aabb_max.x, p.x),
                         std::max(part.aabb_max.y, p.y),
                         std::max(part.aabb_max.z, p.z)};
    }
    for (const Vec3& p : l.v) {
        part.vertices.push_back(p - part.com);
    }
    for (const auto& f : l.faces) {
        part.face_counts.push_back(static_cast<std::uint32_t>(f.size()));
        part.face_indices.insert(part.face_indices.end(), f.begin(), f.end());
    }
    return part;
}

// A regular n-gon ring of radius r in the horizontal plane at height y.
[[nodiscard]] std::vector<Vec3> ring(float r, float y, unsigned n) {
    std::vector<Vec3> out;
    for (unsigned k = 0; k < n; ++k) {
        const float a = (static_cast<float>(k) + 0.5f) * 2.0f * kPi / static_cast<float>(n);
        out.push_back({r * std::cos(a), y, r * std::sin(a)});
    }
    return out;
}

// A square ring of half-width h centred on `c`, lying in the plane perpendicular to `dir`.
[[nodiscard]] std::vector<Vec3> square_ring(Vec3 c, Vec3 dir, float h) {
    const Vec3 u{1.0f, 0.0f, 0.0f};
    const Vec3 w = core::normalize(core::cross(dir, u));
    return {c + u * h + w * h, c - u * h + w * h, c - u * h - w * h, c + u * h - w * h};
}

// An axis-aligned box ring (x0..x1, z0..z1) at height y.
[[nodiscard]] std::vector<Vec3> rect_ring(float x0, float x1, float z0, float z1, float y) {
    return {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
}

// Lamp dimensions, in the pattern's own frame: origin at the foot of the pole, +Y up, the arm
// reaching out along +Z (toward the road). One set of numbers shared by the geometry and the spot
// light so the light always hangs exactly under the luminaire.
constexpr float kPoleHeight = 3.95f;
constexpr float kPoleBaseRadius = 0.11f;
constexpr float kPoleTopRadius = 0.065f;
constexpr Vec3 kArmStart{0.0f, 3.78f, 0.075f};
constexpr Vec3 kArmEnd{0.0f, 4.20f, 1.30f};
constexpr float kArmHalf = 0.04f;
constexpr Vec3 kHeadCentre{0.0f, 4.13f, 1.28f};
constexpr Vec3 kHeadHalf{0.15f, 0.035f, 0.28f};
constexpr Vec3 kLightOffset{0.0f, 4.08f, 1.28f}; // just under the luminaire

[[nodiscard]] core::Quat yaw(float radians) {
    return core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, radians);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Where the dressed environment already is
//
// `play_environment.cpp` places its trees, verges, dirt paths and pond at literal coordinates, and
// this brick does not own that file. Rather than let a building grow through a tree, the lot rule
// below carries the same coordinates. They are duplicated ON PURPOSE and checked in the smoke
// (`lot_clear` is public): if the environment moves a tree, the proof that no lot overlaps it is
// the thing that should go red, not a screenshot someone has to notice.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
constexpr std::array<std::array<float, 2>, 26> kTrees{
    {{-8, -7},  {-8, 8},   {10, -8},  {22, 8},   {34, -8},  {46, 8},    {-10, 20},
     {0, 18},   {12, 20},  {24, 18},  {38, 22},  {50, 20},  {-10, -20}, {4, -18},
     {18, -20}, {30, -18}, {44, -20}, {54, -22}, {64, 14},  {68, -16},  {80, 22},
     {92, -26}, {10, 48},  {28, 52},  {42, -50}, {-22, -48}}};
constexpr float kBuildingHalf = 4.0f;  // footprint 8 m
constexpr float kTreeClearance = 3.4f; // canopy reach beyond the footprint edge
constexpr float kLotPitch = 12.0f;     // footprint + 4 m gap, as the original street

} // namespace

bool lot_clear(float x, float z) {
    // The original block, its street and a margin: the 8 authored buildings stay untouched.
    if (x + kBuildingHalf > -2.0f && x - kBuildingHalf < 46.0f && z + kBuildingHalf > -16.0f &&
        z - kBuildingHalf < 16.0f) {
        return false;
    }
    for (const auto& t : kTrees) {
        const float dx = std::max(std::abs(t[0] - x) - kBuildingHalf, 0.0f);
        const float dz = std::max(std::abs(t[1] - z) - kBuildingHalf, 0.0f);
        if (std::sqrt(dx * dx + dz * dz) < kTreeClearance) {
            return false;
        }
    }
    // The grass verge (and so the dirt paths and 1,800 grass tufts that live on it): two strips
    // 76 m x 23.6 m centred at x=22, z=+-26.2. The pond sits inside the north one.
    for (const float s : {-1.0f, 1.0f}) {
        const float zc = s * 26.2f;
        if (x + kBuildingHalf > -17.0f && x - kBuildingHalf < 61.0f &&
            z + kBuildingHalf > zc - 12.8f && z - kBuildingHalf < zc + 12.8f) {
            return false;
        }
    }
    return true;
}

assets::DestructibleAsset make_lamp_asset() {
    assets::DestructibleAsset a;
    // Pole: an octagonal frustum, fat at the foot and slim at the top. Tapering is what makes a
    // lamp read as a lamp rather than a stick, and it is free for a lofted hull.
    a.parts.push_back(
        make_part(loft(ring(kPoleBaseRadius, 0.0f, 8), ring(kPoleTopRadius, kPoleHeight, 8))));
    // Arm: a thin square tube from just beside the pole top, rising slightly as it reaches out.
    {
        const Vec3 dir = core::normalize(kArmEnd - kArmStart);
        a.parts.push_back(make_part(
            loft(square_ring(kArmStart, dir, kArmHalf), square_ring(kArmEnd, dir, kArmHalf))));
    }
    // Luminaire: a thin slab hung under the arm's end. Thin on purpose: it is the part a bullet
    // is most likely to find, and it should be cheap to knock out.
    a.parts.push_back(make_part(loft(rect_ring(kHeadCentre.x - kHeadHalf.x,
                                               kHeadCentre.x + kHeadHalf.x,
                                               kHeadCentre.z - kHeadHalf.z,
                                               kHeadCentre.z + kHeadHalf.z,
                                               kHeadCentre.y - kHeadHalf.y),
                                     rect_ring(kHeadCentre.x - kHeadHalf.x,
                                               kHeadCentre.x + kHeadHalf.x,
                                               kHeadCentre.z - kHeadHalf.z,
                                               kHeadCentre.z + kHeadHalf.z,
                                               kHeadCentre.y + kHeadHalf.y))));

    // A chain pole -- arm -- head. Bond strength is informational in this engine (the support
    // solve is pure connectivity), so it is the shared-face area, as the cook writes it.
    a.bonds = {{0, 1, 0.01f}, {1, 2, 0.02f}};
    a.anchors = {0}; // the pole is planted; cut it and the whole fixture comes down together
    a.half_extents = {0.15f, 2.15f, 0.7f};
    // Tuned against the cooked crate (threshold 8, scale 0.02, 4 parts), scaled by the much
    // smaller mass: a lamp should fall to a few rifle hits, not to a footstep. INFERRED, not
    // measured -- there is no lamp-specific damage data yet.
    a.damage_threshold = 3.0f;
    a.damage_scale = 0.05f;
    return a;
}

assets::DestructibleAsset make_barrier_asset() {
    assets::DestructibleAsset a;
    // A New-Jersey-style road barrier: 0.5 m at the foot narrowing to 0.2 m at the top, 1 m high,
    // 2 m long, split down the middle into two parts. The split plane is shared, so the two
    // hulls butt without overlapping (overlapping hulls fling apart when they are freed).
    for (int half = 0; half < 2; ++half) {
        const float x0 = half == 0 ? -1.0f : 0.0f;
        const float x1 = half == 0 ? 0.0f : 1.0f;
        a.parts.push_back(make_part(
            loft(rect_ring(x0, x1, -0.25f, 0.25f, 0.0f), rect_ring(x0, x1, -0.1f, 0.1f, 1.0f))));
    }
    a.bonds = {{0, 1, 0.4f}};
    a.anchors = {0}; // one half is bedded in; lose it and the other half drops
    a.half_extents = {1.0f, 0.5f, 0.25f};
    // Concrete: heavy, so it shrugs off what would fell a lamp (the cooked 28 for a hero wall
    // against 8 for a crate sets the scale). INFERRED.
    a.damage_threshold = 20.0f;
    a.damage_scale = 0.01f;
    return a;
}

namespace {

// Pull a prop's placement out before it is despawned.
struct Prop {
    ecs::Entity entity{};
    core::Transform tf{};
};

} // namespace

ExpandStats expand(ecs::World& world) {
    using blockkit::SlabRole;
    using ecs::LocalTransform;
    namespace sk = blockkit::slab_kind;

    ExpandStats stats;
    const blockkit::BlockParams p;

    // ── 1. Find what is being replaced, BEFORE changing anything ──────────────────────────────
    // Collect first, mutate after: a structural change inside a query is forbidden.
    std::vector<Prop> masts, barriers, lights;
    ecs::Entity ground_entity = ecs::kNullEntity;
    world.query<SlabRole, LocalTransform>().for_each(
        [&](ecs::Entity e, SlabRole& role, LocalTransform& lt) {
            switch (role.kind) {
                case sk::kLampMast:
                    masts.push_back({e, lt.value});
                    break;
                case sk::kBarrier:
                    barriers.push_back({e, lt.value});
                    break;
                case sk::kLight:
                    if (role.building == blockkit::kNoBuilding) {
                        lights.push_back({e, lt.value});
                    }
                    break;
                case sk::kStreet:
                    ground_entity = e;
                    break;
                default:
                    break;
            }
        });
    // Heads are not needed: they go with their masts. Despawn them by role below.
    std::vector<ecs::Entity> heads;
    world.query<SlabRole>().for_each([&](ecs::Entity e, SlabRole& role) {
        if (role.kind == sk::kLampHead) {
            heads.push_back(e);
        }
    });

    // Deterministic numbering, independent of archetype/insertion order: lamps by (z, x), barriers
    // by x. The same index must come out on both peers and on a re-run, because the index is what
    // the replicated role carries.
    std::sort(masts.begin(), masts.end(), [](const Prop& a, const Prop& b) {
        if (a.tf.translation.z != b.tf.translation.z) {
            return a.tf.translation.z < b.tf.translation.z;
        }
        return a.tf.translation.x < b.tf.translation.x;
    });
    std::sort(barriers.begin(), barriers.end(), [](const Prop& a, const Prop& b) {
        return a.tf.translation.x < b.tf.translation.x;
    });

    // ── 2. Lamps: three destructible parts, and the existing spot light follows the luminaire ────
    for (std::size_t i = 0; i < masts.size(); ++i) {
        const Vec3 at = masts[i].tf.translation;
        // The arm reaches toward the road: +Z for the lamp on the -Z kerb, -Z for the other.
        const core::Quat facing = yaw(at.z > 0.0f ? kPi : 0.0f);
        core::Transform tf;
        tf.translation = {at.x, 0.0f, at.z};
        tf.rotation = facing;
        // `kCrate` as the slab kind is deliberate. It is destructible (so it binds and replicates
        // like any cooked slab) and the palette gives it a material but NO mesh, so the slab is
        // never itself drawn -- only its parts are. The per-part look is applied by Dresser.
        (void)world.spawn_with(LocalTransform{tf},
                               SlabRole{kLampTag | static_cast<std::uint32_t>(i), 0, sk::kCrate, 0},
                               destruction::Destructible{kLampAsset});

        // Re-aim the spot light that stood on this mast: nearest light in plan view.
        std::size_t best = lights.size();
        float best_d = 1e30f;
        for (std::size_t k = 0; k < lights.size(); ++k) {
            const float dx = lights[k].tf.translation.x - at.x;
            const float dz = lights[k].tf.translation.z - at.z;
            if (dx * dx + dz * dz < best_d) {
                best_d = dx * dx + dz * dz;
                best = k;
            }
        }
        if (best < lights.size()) {
            if (auto* role = world.get<SlabRole>(lights[best].entity)) {
                role->building = kLampTag | static_cast<std::uint32_t>(i);
            }
            if (auto* lt = world.get<LocalTransform>(lights[best].entity)) {
                lt->value.translation = tf.translation + core::rotate(facing, kLightOffset);
            }
        }
        ++stats.lamps;
    }
    for (const Prop& m : masts) {
        world.despawn(m.entity);
    }
    for (const ecs::Entity h : heads) {
        world.despawn(h);
    }

    // ── 3. Barriers: two destructible parts each ──────────────────────────────────────────────
    for (std::size_t i = 0; i < barriers.size(); ++i) {
        core::Transform tf;
        tf.translation = {barriers[i].tf.translation.x, 0.0f, barriers[i].tf.translation.z};
        (void)world.spawn_with(
            LocalTransform{tf},
            SlabRole{kBarrierTag | static_cast<std::uint32_t>(i), 0, sk::kCrate, 0},
            destruction::Destructible{kBarrierAsset});
        world.despawn(barriers[i].entity);
        ++stats.barriers;
    }

    // ── 4. The new buildings ──────────────────────────────────────────────────────────────────
    // Same prefab as the street: hollow slabs from the existing background cooks, a doorway
    // gap in the ground storey, two storeys instead of three. NO interior point lights: each
    // light is a per-pixel cost on every cluster it touches, 24 buildings x 2 would add 48 for
    // rooms nobody sees into, and that is exactly the kind of growth that brings frame stalls
    // back. Interiors of background buildings are dark by design.
    const blockkit::CookSpec* wall = blockkit::find_cook("wall_bg");
    const blockkit::CookSpec* side = blockkit::find_cook("side_bg");
    const blockkit::CookSpec* floor = blockkit::find_cook("floor_bg");
    const blockkit::CookSpec* half = blockkit::find_cook("half_bg");

    constexpr std::uint32_t kStoreys = 2;
    const float half_f = p.footprint * 0.5f;
    const float inset = half_f - p.slab_thickness * 0.5f;
    const float half_wall_width = (p.footprint - p.doorway_width) * 0.5f;

    float max_x = p.street_length();
    std::uint32_t built = 0;
    // Lots are walked in a regular grid, nearest first, and each is TESTED rather than assumed:
    // rows at |z| = 10 (facing the road, like the originals) and |z| = 22 (a second rank
    // behind them), one lot pitch apart. Whatever the dressed environment occupies is skipped.
    for (int column = 0; built < kExtraBuildings && column < 64; ++column) {
        const float x = 76.0f + static_cast<float>(column) * kLotPitch;
        for (const float z : {-10.0f, 10.0f, -22.0f, 22.0f}) {
            if (built >= kExtraBuildings) {
                break;
            }
            if (!lot_clear(x, z)) {
                ++stats.lots_rejected;
                continue;
            }
            const std::uint32_t b = p.building_count() + built;
            core::Transform frame;
            frame.translation = {x, 0.0f, z};
            frame.rotation = yaw(z < 0.0f ? 0.0f : kPi);
            const std::uint32_t tint = b % static_cast<std::uint32_t>(blockkit::kTintCount);

            const auto slab = [&](core::Transform local,
                                  std::uint32_t storey,
                                  std::uint32_t kind,
                                  const blockkit::CookSpec& spec) {
                (void)world.spawn_with(LocalTransform{frame * local},
                                       SlabRole{b, storey, kind, tint},
                                       destruction::Destructible{spec.asset});
            };
            for (std::uint32_t s = 0; s < kStoreys; ++s) {
                const float wall_y =
                    static_cast<float>(s) * p.storey_height + p.storey_height * 0.5f;
                if (s == 0) {
                    for (const float sign : {-1.0f, 1.0f}) {
                        core::Transform t;
                        t.translation = {sign * (half_f - half_wall_width * 0.5f), wall_y, inset};
                        slab(t, s, sk::kHalfWall, *half);
                    }
                } else {
                    core::Transform t;
                    t.translation = {0.0f, wall_y, inset};
                    slab(t, s, sk::kWall, *wall);
                }
                {
                    core::Transform t;
                    t.translation = {0.0f, wall_y, -inset};
                    slab(t, s, sk::kWall, *wall);
                }
                for (const float sign : {-1.0f, 1.0f}) {
                    core::Transform t;
                    t.translation = {sign * inset, wall_y, 0.0f};
                    t.rotation = yaw(kPi * 0.5f);
                    slab(t, s, sk::kSideWall, *side);
                }
                {
                    const bool roof = s + 1u == kStoreys;
                    core::Transform t;
                    t.translation = {0.0f, static_cast<float>(s + 1) * p.storey_height, 0.0f};
                    slab(t,
                         roof ? blockkit::kRoofStorey : s + 1u,
                         roof ? sk::kRoof : sk::kFloor,
                         *floor);
                }
            }
            max_x = std::max(max_x, x + half_f);
            ++built;
        }
    }
    stats.buildings_added = built;

    // ── 5. The road reaches the new quarter: kerbs, and ONE enlarged ground surface ────────────
    // The extension of the road is the same two kerb strips the street has, continued.
    const float old_len = p.street_length();
    const float kerb_len = max_x + 4.0f - old_len;
    if (kerb_len > 0.0f) {
        for (const float sign : {-1.0f, 1.0f}) {
            core::Transform t;
            t.translation = {
                old_len + kerb_len * 0.5f, 0.075f, sign * (p.street_width * 0.5f - 0.2f)};
            t.scale = {kerb_len, 0.15f, 0.4f};
            (void)world.spawn_with(LocalTransform{t},
                                   SlabRole{blockkit::kNoBuilding, 0, sk::kKerb, 0});
            ++stats.kerbs_added;
        }
    }

    // ONE surface, grown -- not one per block. `PlayWorld::street_config` demands exactly one
    // GroundSurface and derives the terrain's flattened street rectangle from it, so growing this
    // component is also what makes the streamed heightfield flat under the new quarter: the
    // collider, the drawn mesh, the GI proxy and the flat terrain all read the same numbers.
    // Its west and Z extents are unchanged (the dressed verges are laid against them); only the
    // east edge moves. The cell size stays 4 m, which keeps the mesh at ~1 quad per 16 m^2.
    if (ground_entity != ecs::kNullEntity) {
        auto* surface = world.get<ground::GroundSurface>(ground_entity);
        auto* lt = world.get<LocalTransform>(ground_entity);
        if (surface != nullptr && lt != nullptr) {
            const float west = lt->value.translation.x - surface->half_x;
            const float east = max_x + 16.0f; // a margin of road past the last building
            const float centre = (west + east) * 0.5f;
            lt->value.translation.x = centre;
            surface->half_x = (east - west) * 0.5f;
            surface->cells_x = static_cast<std::uint32_t>(
                std::max(1.0f, std::round(surface->half_x * 2.0f / surface->tile_metres)));
            stats.ground_half_x = surface->half_x;
            stats.ground_half_z = surface->half_z;
        }
    }
    return stats;
}

bool register_patterns(destruction::DestructionWorld& destruction,
                       physics::PhysicsWorld& physics,
                       std::unordered_map<std::uint64_t, destruction::PatternId>& out,
                       destruction_render::PartLeafRenderer* leaves,
                       render::MeshRegistry* meshes) {
    const std::array<std::pair<std::uint64_t, assets::DestructibleAsset>, 2> all{
        {{kLampAsset, make_lamp_asset()}, {kBarrierAsset, make_barrier_asset()}}};
    for (const auto& [id, asset] : all) {
        const destruction::PatternId pid = destruction.register_pattern(asset, physics);
        if (!pid.is_valid()) {
            return false;
        }
        out[id] = pid;
        if (leaves != nullptr) {
            (void)leaves->register_pattern(pid, asset, meshes);
        }
    }
    return true;
}

void Dresser::init(const blockkit::BlockPalette& palette, render::MaterialRegistry& materials) {
    palette_ = palette;
    // The luminaire after it is broken: the same dark housing, with the glow taken out. Without
    // this a knocked-off head would lie in the road still shining like a lit lamp.
    render::PbrMaterialDesc dead = materials.get(palette.lamp_head);
    dead.emissive[0] = dead.emissive[1] = dead.emissive[2] = 0.0f;
    dead_head_ = materials.add(dead);
    ready_ = true;
}

SyncStats Dresser::sync(ecs::World& world,
                        const destruction::DestructionWorld& destruction,
                        const destruction_render::PartLeafRenderer& leaves) {
    using blockkit::SlabRole;
    SyncStats s;

    // lit[i]: 1 = the lamp's luminaire is still standing, 0 = broken, 2 = no instance yet (a
    // client waiting for replication keeps the authored light rather than blinking it off).
    std::array<std::uint8_t, 0x100> lit{};
    lit.fill(2);

    const auto paint = [&](ecs::Entity leaf, render::MaterialId m) {
        if (leaf == ecs::kNullEntity) {
            return; // retired by the visual budget
        }
        if (auto* mat = world.get<render::MaterialRef>(leaf)) {
            if (mat->material != m) {
                mat->material = m;
            }
            ++s.leaves_dressed;
        }
    };

    world.query<SlabRole, destruction::DestructibleInstanceRef>().for_each(
        [&](SlabRole& role, destruction::DestructibleInstanceRef& ref) {
            const std::uint32_t tag = role.building & kTagMask;
            if ((tag != kLampTag && tag != kBarrierTag) ||
                ref.instance == destruction::kUnboundInstance) {
                return;
            }
            const destruction::InstanceId inst{ref.instance};
            const std::vector<ecs::Entity> parts = leaves.leaves_of(inst);
            if (tag == kLampTag) {
                if (parts.size() != 3) {
                    return;
                }
                const std::uint32_t index = role.building & 0xFFu;
                const bool head_up = destruction.part_alive(inst, kLampHead);
                paint(parts[kLampPole], palette_.lamp_mast);
                paint(parts[kLampArm], palette_.lamp_mast);
                paint(parts[kLampHead], head_up ? palette_.lamp_head : dead_head_);
                lit[index] = head_up ? 1 : 0;
                ++s.lamps_seen;
                s.heads_broken += head_up ? 0u : 1u;
            } else {
                for (const ecs::Entity leaf : parts) {
                    paint(leaf, palette_.barrier);
                }
                ++s.barriers_seen;
            }
        });

    // The lights are level geometry each peer holds itself, so they are found by the tag the
    // authoring step wrote on them. Removing the component, rather than zeroing its intensity,
    // matters: a zero-intensity spot would still occupy one of the 8 local shadow slots and be
    // rendered into a shadow map for nothing.
    struct Toggle {
        ecs::Entity e;
        bool on;
    };

    std::vector<Toggle> toggles;
    world.query<SlabRole>().for_each([&](ecs::Entity e, SlabRole& role) {
        if ((role.building & kTagMask) != kLampTag || role.kind != blockkit::slab_kind::kLight) {
            return;
        }
        const std::uint8_t state = lit[role.building & 0xFFu];
        if (state == 2) {
            return;
        }
        const bool has = world.has<render::SpotLight>(e);
        if (state == 1) {
            ++s.lamps_lit;
        }
        if ((state == 1) != has) {
            toggles.push_back({e, state == 1});
        }
    });
    for (const Toggle& t : toggles) {
        if (t.on) {
            (void)world.add_component(t.e,
                                      render::SpotLight{blockkit::kLampColor[0],
                                                        blockkit::kLampColor[1],
                                                        blockkit::kLampColor[2],
                                                        blockkit::kLampIntensity,
                                                        blockkit::kLampRange,
                                                        blockkit::kLampInnerAngle,
                                                        blockkit::kLampOuterAngle});
        } else {
            (void)world.remove_component<render::SpotLight>(t.e);
        }
        ++s.lights_toggled;
    }
    last_ = s;
    return s;
}

} // namespace rime::block_play::city
