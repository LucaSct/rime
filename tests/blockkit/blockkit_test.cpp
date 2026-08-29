// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.2c — the block, assembled. The content half of ADR-0035 §1's scale floors, proven GPU-free.
//
// WHAT THIS PROVES, and the discipline behind each case:
//
//   * The generator is DETERMINISTIC — same parameters, byte-identical `.rscene`. Nothing generated
//     is committed to the repo, so reproducibility is the only thing standing between a build-time
//     artefact and an unreviewable one.
//   * The scene ROUND-TRIPS — what loads back is what was assembled, checked placement by placement
//     rather than by entity count. A count-only check passes happily when 213 entities load as 213
//     wrong entities.
//   * ADR-0035 §1's floors are measured FROM THE LOADED WORLD, never from the generator's intent.
//     `predict()` derives the same numbers from the parameters by a different route, and the two
//     are compared — two independent derivations agreeing is worth more than one number reported
//     by the code that produced it.
//   * Every role the palette meets gets a material, and a role it does not know is a RED COUNT.
//     An unhandled kind is otherwise an invisible prop: green in a headless proof, and missing in a
//     screenshot nobody takes.
//
// The scene format is the reason none of this needs a GPU: it carries placement and intent, and the
// look is derived (blockkit/role.hpp). Meshes are the only GPU-bound half, and they are a separate
// call for exactly that reason.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "rime/blockkit/block.hpp"
#include "rime/blockkit/palette.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/passes.hpp"
#include "rime/scene/scene_format.hpp"

using namespace rime;

namespace {

// Register the component set a block scene names, exactly as any loader must.
void register_block_components(ecs::World& world) {
    ecs::register_transform_components(world);
    blockkit::register_blockkit_components(world);
    destruction::register_destruction_components(world);
    render::register_render_components(world);
}

// A placement + role, keyed so two worlds can be compared without depending on entity ids matching.
struct Placement {
    core::Vec3 translation{};
    core::Vec3 scale{};
    blockkit::SlabRole role{};
    std::uint64_t asset = 0;
};

[[nodiscard]] std::string key(const Placement& p) {
    // Rounded to a millimetre: the comparison is "the same slab is in the same place", not a float
    // equality test on numbers that went through a decimal text round trip.
    char buf[192];
    std::snprintf(buf,
                  sizeof(buf),
                  "%d/%d/%d|%d/%d/%d|%u/%u/%u/%u|%llu",
                  static_cast<int>(std::lround(p.translation.x * 1000.0f)),
                  static_cast<int>(std::lround(p.translation.y * 1000.0f)),
                  static_cast<int>(std::lround(p.translation.z * 1000.0f)),
                  static_cast<int>(std::lround(p.scale.x * 1000.0f)),
                  static_cast<int>(std::lround(p.scale.y * 1000.0f)),
                  static_cast<int>(std::lround(p.scale.z * 1000.0f)),
                  p.role.building,
                  p.role.storey,
                  p.role.kind,
                  p.role.tint,
                  static_cast<unsigned long long>(p.asset));
    return buf;
}

[[nodiscard]] std::map<std::string, int> census(ecs::World& world) {
    std::map<std::string, int> out;
    world.query<ecs::LocalTransform, blockkit::SlabRole>().for_each(
        [&](ecs::Entity e, ecs::LocalTransform& tf, blockkit::SlabRole& role) {
            Placement p;
            p.translation = tf.value.translation;
            p.scale = tf.value.scale;
            p.role = role;
            if (const auto* d = world.get<destruction::Destructible>(e)) {
                p.asset = d->asset;
            }
            ++out[key(p)];
        });
    return out;
}

// What the loaded scene actually asks for, counted from its own components.
struct Census {
    std::size_t entities = 0;
    std::size_t destructibles = 0;
    std::size_t parts = 0;
    std::size_t unknown_assets = 0; // a Destructible naming a cook that does not exist
    std::size_t point_lights = 0;
    std::size_t spot_lights = 0;
    std::size_t dir_lights = 0;
    std::size_t structures = 0; // distinct buildings with at least one slab
};

[[nodiscard]] Census measure(ecs::World& world) {
    Census c;
    std::vector<bool> seen_building;

    world.query<blockkit::SlabRole>().for_each([&](ecs::Entity e, blockkit::SlabRole& role) {
        ++c.entities;
        if (const auto* d = world.get<destruction::Destructible>(e)) {
            ++c.destructibles;
            if (const blockkit::CookSpec* spec = blockkit::find_cook_by_asset(d->asset)) {
                c.parts += spec->parts;
            } else {
                ++c.unknown_assets;
            }
        }
        if (role.building != blockkit::kNoBuilding) {
            if (seen_building.size() <= role.building) {
                seen_building.resize(role.building + 1, false);
            }
            if (!seen_building[role.building] && blockkit::slab_kind::is_destructible(role.kind)) {
                seen_building[role.building] = true;
                ++c.structures;
            }
        }
        if (world.get<render::PointLight>(e) != nullptr) {
            ++c.point_lights;
        }
        if (world.get<render::SpotLight>(e) != nullptr) {
            ++c.spot_lights;
        }
        if (world.get<render::DirectionalLight>(e) != nullptr) {
            ++c.dir_lights;
        }
    });
    return c;
}

} // namespace

TEST_CASE("blockkit: assembly is deterministic and round-trips through .rscene") {
    const blockkit::BlockParams params;

    ecs::World a;
    ecs::World b;
    const blockkit::BlockStats sa = blockkit::assemble(a, params);
    const blockkit::BlockStats sb = blockkit::assemble(b, params);
    CHECK(sa.entities == sb.entities);

    const std::string text_a = scene::save_scene_to_string(a);
    const std::string text_b = scene::save_scene_to_string(b);

    // Byte-identical, not merely equivalent. The crate scatter is a stateless integer hash for
    // exactly this reason — a PRNG object would make the i-th crate depend on how many came before
    // it, and the file would drift the moment a count changed anywhere upstream.
    CHECK(text_a == text_b);
    CHECK(text_a.size() > 1000);

    SUBCASE("what loads back is what was assembled") {
        ecs::World loaded;
        register_block_components(loaded);
        const scene::LoadReport report = scene::load_scene_from_string(loaded, text_a);
        REQUIRE(report.ok);
        CHECK(report.entities == sa.entities);

        // Placement by placement, keyed on transform + role + asset. An entity-count check alone
        // passes when 213 entities load as 213 wrong entities.
        CHECK(census(loaded) == census(a));
    }
}

TEST_CASE("blockkit: the loaded block meets ADR-0035 §1's scale floors") {
    const blockkit::BlockParams params;
    ecs::World world;
    const blockkit::BlockStats assembled = blockkit::assemble(world, params);

    // Route one: what the prefab says it spawned. Route two: what the parameters imply, derived
    // without a world. They agree only if the prefab wired up what it meant to.
    const blockkit::BlockStats expected = blockkit::predict(params);
    CHECK(assembled.entities == expected.entities);
    CHECK(assembled.parts == expected.parts);
    CHECK(assembled.slabs == expected.slabs);
    CHECK(assembled.crates == expected.crates);
    CHECK(assembled.props == expected.props);
    CHECK(assembled.point_lights == expected.point_lights);
    CHECK(assembled.spot_lights == expected.spot_lights);

    // Route three, and the one that counts: measured from a world that came back off disk, using
    // only the components the file carries.
    ecs::World loaded;
    register_block_components(loaded);
    REQUIRE(scene::load_scene_from_string(loaded, scene::save_scene_to_string(world)).ok);
    const Census c = measure(loaded);

    // An asset id with no cook behind it would otherwise show up as a quietly smaller block.
    CHECK(c.unknown_assets == 0);
    CHECK(c.destructibles == assembled.destructibles());
    CHECK(c.parts == assembled.parts);

    // ── The floors themselves (ADR-0035 §1) ──────────────────────────────────────────────────────
    CHECK(c.structures >= 8);
    CHECK(c.parts >= 1500);
    CHECK(c.point_lights + c.spot_lights >= 32);
    CHECK(c.dir_lights >= 1);

    // And the exact numbers, so a change that keeps clearing the floor by less is visible in a diff
    // rather than silently absorbed.
    CHECK(c.structures == 8);
    CHECK(c.destructibles == 140);
    CHECK(c.parts == 2016);
    CHECK(c.point_lights == 24);
    CHECK(c.spot_lights == 12);
    CHECK(c.dir_lights == 1);

    // The lighting rig is only affordable because m10.3 exists: the ADR-0022 uniform-block path
    // caps at kMaxPointLights, and this block is well past it. If that ever stops being true the
    // demo must turn clustered forward on anyway, so assert the reason rather than the flag.
    CHECK(c.point_lights + c.spot_lights > render::kMaxPointLights);
}

TEST_CASE("blockkit: the palette covers every role it is handed") {
    ecs::World world;
    (void)blockkit::assemble(world);

    render::MaterialRegistry materials;
    const blockkit::BlockPalette palette = blockkit::build_palette(materials);

    // Four facades, four bands, and seven singletons.
    CHECK(palette.material_count == 15);
    CHECK(materials.size() == palette.material_count);

    // Mesh ids stay invalid without a device — the GPU-free half of the palette, deliberately.
    CHECK(palette.unit_cube == render::kInvalidMeshId);
    CHECK(palette.street_plane == render::kInvalidMeshId);

    const blockkit::PaletteStats stats = blockkit::apply_palette(world, palette);

    // The counter that matters: a role the palette has no answer for is an invisible prop, and an
    // invisible prop is green in a headless proof.
    CHECK(stats.missing == 0);

    // Every non-light role got one, and nothing was skipped silently.
    std::size_t geometry_roles = 0;
    std::size_t unmateraled = 0;
    world.query<blockkit::SlabRole>().for_each([&](ecs::Entity e, blockkit::SlabRole& role) {
        if (role.kind == blockkit::slab_kind::kLight) {
            return;
        }
        ++geometry_roles;
        if (world.get<render::MaterialRef>(e) == nullptr) {
            ++unmateraled;
        }
    });
    CHECK(unmateraled == 0);
    CHECK(stats.materialed == geometry_roles);

    // No prop got a mesh, because none could be uploaded.
    CHECK(stats.meshed == 0);

    SUBCASE("the ground storey wears the band and the storeys above wear the facade") {
        std::size_t banded = 0;
        std::size_t faced = 0;
        world.query<blockkit::SlabRole, render::MaterialRef>().for_each(
            [&](blockkit::SlabRole& role, render::MaterialRef& mat) {
                const bool wall = role.kind == blockkit::slab_kind::kWall ||
                                  role.kind == blockkit::slab_kind::kSideWall ||
                                  role.kind == blockkit::slab_kind::kHalfWall;
                if (!wall) {
                    return;
                }
                const std::size_t tint = role.tint % blockkit::kTintCount;
                if (role.storey == 0) {
                    CHECK(mat.material == palette.band[tint]);
                    ++banded;
                } else {
                    CHECK(mat.material == palette.facade[tint]);
                    ++faced;
                }
            });
        // Per building: 5 walls on the ground storey (two front halves, a back, two sides) and 4 on
        // each storey above.
        CHECK(banded == 8u * 5u);
        CHECK(faced == 8u * 4u * 2u);
    }

    SUBCASE("re-applying is idempotent, which is what makes re-tinting live") {
        const blockkit::PaletteStats again = blockkit::apply_palette(world, palette);
        CHECK(again.materialed == stats.materialed);
        CHECK(again.missing == 0);
        CHECK(materials.size() == palette.material_count); // no ids minted on a second pass
    }
}

TEST_CASE("blockkit: geometry butts rather than overlaps") {
    // The reason there are nine cooks and not five. Overlapping static slabs are invisible while
    // everything stands and are exactly wrong at the moment the demo is showing off: once the bonds
    // break, interpenetrating hulls resolve by flinging apart.
    const blockkit::BlockParams p;

    const blockkit::CookSpec* wall = blockkit::find_cook("wall_bg");
    const blockkit::CookSpec* side = blockkit::find_cook("side_bg");
    const blockkit::CookSpec* floor = blockkit::find_cook("floor_bg");
    const blockkit::CookSpec* half = blockkit::find_cook("half_bg");
    REQUIRE(wall != nullptr);
    REQUIRE(side != nullptr);
    REQUIRE(floor != nullptr);
    REQUIRE(half != nullptr);

    // Front/back walls span the footprint; side walls and floor slabs are inset by one slab
    // thickness at each end so they land between their neighbours.
    CHECK(wall->size_x == doctest::Approx(p.footprint));
    CHECK(side->size_x == doctest::Approx(p.footprint - 2.0f * p.slab_thickness));
    CHECK(floor->size_x == doctest::Approx(p.footprint - 2.0f * p.slab_thickness));
    CHECK(floor->size_z == doctest::Approx(p.footprint - 2.0f * p.slab_thickness));

    // Two halves plus the doorway make one front wall. `rime fracture` cooks boxes only, so an
    // opening can be authored only as space left between slabs.
    CHECK(2.0f * half->size_x + p.doorway_width == doctest::Approx(p.footprint));

    // Every slab is exactly one storey tall, and every cook is the same thickness.
    CHECK(wall->size_y == doctest::Approx(p.storey_height));
    CHECK(side->size_y == doctest::Approx(p.storey_height));
    CHECK(wall->size_z == doctest::Approx(p.slab_thickness));
    CHECK(floor->size_y == doctest::Approx(p.slab_thickness));

    // Hero cooks are the same GEOMETRY at a finer fracture — the two buildings differ in how they
    // break, not in how big they are, so the scale is not distorted to satisfy a debris counter.
    for (const char* pair : {"wall", "side", "floor", "half"}) {
        const blockkit::CookSpec* bg = blockkit::find_cook(std::string(pair) + "_bg");
        const blockkit::CookSpec* hr = blockkit::find_cook(std::string(pair) + "_hero");
        REQUIRE(bg != nullptr);
        REQUIRE(hr != nullptr);
        CHECK(bg->size_x == doctest::Approx(hr->size_x));
        CHECK(bg->size_y == doctest::Approx(hr->size_y));
        CHECK(bg->size_z == doctest::Approx(hr->size_z));
        CHECK(hr->parts > bg->parts);
        CHECK(bg->seed != hr->seed);
        CHECK(bg->asset != hr->asset);
    }
}

TEST_CASE("blockkit: the street puts the two clients at opposite ends") {
    const blockkit::BlockParams p;

    // 4 footprints and 3 gaps.
    CHECK(p.street_length() == doctest::Approx(44.0f));
    CHECK(p.slabs_per_building() == 16u);
    CHECK(p.building_count() == 8u);

    const core::Vec3 west = blockkit::west_viewpoint(p);
    const core::Vec3 east = blockkit::east_viewpoint(p);
    CHECK(west.x < 0.0f);
    CHECK(east.x > p.street_length());
    CHECK(east.x - west.x > p.street_length());
    CHECK(west.y == doctest::Approx(blockkit::kEyeHeight));
    CHECK(east.y == doctest::Approx(blockkit::kEyeHeight));

    // Both stand on the street's centre line, looking along it: that is what makes their relevancy
    // sets differ (M11.5) and gives the m13.2a frustum cull something to reject.
    CHECK(west.z == doctest::Approx(0.0f));
    CHECK(east.z == doctest::Approx(0.0f));
}

TEST_CASE("blockkit: cook ids are unique, and every slab names one") {
    std::map<std::uint64_t, int> by_asset;
    std::map<std::string, int> by_name;
    for (const blockkit::CookSpec& c : blockkit::cook_specs()) {
        ++by_asset[c.asset];
        ++by_name[c.name];
        CHECK(c.parts > 0);
        CHECK(c.size_x > 0.0f);
        CHECK(c.size_y > 0.0f);
        CHECK(c.size_z > 0.0f);
        CHECK(blockkit::find_cook_by_asset(c.asset) == &c);
        CHECK(blockkit::find_cook(c.name) == &c);
    }
    CHECK(by_asset.size() == blockkit::cook_specs().size());
    CHECK(by_name.size() == blockkit::cook_specs().size());
    CHECK(blockkit::find_cook_by_asset(0) == nullptr);
    CHECK(blockkit::find_cook("no_such_cook") == nullptr);
}
