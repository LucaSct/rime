// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "character_fixture.hpp"
#include "rime/assets/procedural_terrain.hpp"

// Exercise the shipped movement seam on the shipped LOD0 geometry. The selected routes are
// deterministic uphill paths whose sampled triangle normals are all below the walkable limit.
// This is deliberately a moving capsule proof, rather than only a height/raycast proof.
using namespace rime_test;

namespace {

struct Route {
    core::Vec3 start{};
    core::Vec3 direction{}; // horizontal unit vector
    float rise = -std::numeric_limits<float>::infinity();
    bool found = false;
};

void add_generated_tiles(physics::PhysicsWorld& world, int min_tx, int max_tx) {
    const assets::ProceduralTerrainConfig terrain{}; // same seed and street footprint as sample
    for (int tx = min_tx; tx <= max_tx; ++tx) {
        if (tx > -2 && tx < 2)
            continue;
        for (int tz = -1; tz <= 1; ++tz) {
            const auto tile = assets::generate_procedural_tile(terrain, tx, tz, 0);
            REQUIRE(tile.has_value());
            physics::HeightfieldDesc desc;
            desc.samples = tile->samples;
            desc.columns = tile->columns;
            desc.rows = tile->rows;
            desc.cell_size_x = tile->cell_size_x;
            desc.cell_size_z = tile->cell_size_z;
            desc.height_scale = tile->height_scale;
            desc.height_offset = tile->height_offset;
            desc.thickness = 64.0f;
            const auto id = world.register_heightfield(desc);
            REQUIRE(id.is_valid());
            physics::BodyDesc body;
            body.motion = physics::MotionType::Static;
            body.shape.type = physics::ShapeType::Heightfield;
            body.shape.heightfield = id;
            body.position = tile->origin;
            REQUIRE(world.create_body(body).is_valid());
        }
    }
}

bool support(const physics::PhysicsWorld& world, float x, float z, physics::RayHit& hit) {
    physics::QueryFilter filter;
    filter.dynamics = false;
    return world.raycast({{x, 64.0f, z}, {0.0f, -1.0f, 0.0f}, 128.0f}, hit, filter);
}

bool walkable_route(const physics::PhysicsWorld& world,
                    const core::Vec3 start,
                    const core::Vec3 dir,
                    float& rise) {
    physics::RayHit first;
    if (!support(world, start.x, start.z, first))
        return false;
    physics::RayHit last;
    if (!support(world, start.x + 8.0f * dir.x, start.z + 8.0f * dir.z, last))
        return false;
    rise = last.point.y - first.point.y;
    if (rise <= 0.30f)
        return false;
    for (int i = 0; i <= 32; ++i) {
        const float t = static_cast<float>(i) * 0.25f;
        const float x = start.x + t * dir.x;
        const float z = start.z + t * dir.z;
        if (!support(world, x, z, last) || last.normal.y < 0.73f)
            return false;
    }
    return true;
}

std::array<Route, 6> find_routes(const physics::PhysicsWorld& world) {
    std::array<Route, 6> routes{}; // per west/east region: X, diagonal, X tile seam
    constexpr float kDiag = 0.707106781f;
    const std::array<core::Vec3, 4> directions{
        {{1, 0, 0}, {-1, 0, 0}, {kDiag, 0, kDiag}, {-kDiag, 0, -kDiag}}};
    for (int x = -330; x <= 254; ++x) {
        if (x > -170 && x < 160)
            continue;
        const int region = x < 0 ? 0 : 3;
        for (int z = -30; z <= 30; z += 2) {
            for (int d = 0; d < 4; ++d) {
                const auto dir = directions[d];
                const core::Vec3 start{static_cast<float>(x), 0, static_cast<float>(z)};
                const bool seam =
                    std::floor(start.x / 64.0f) != std::floor((start.x + 8.0f * dir.x) / 64.0f);
                const int kind = d >= 2 ? 1 : (seam ? 2 : 0);
                const int category = region + kind;
                float rise = 0;
                if (!walkable_route(world, start, dir, rise))
                    continue;
                if (!routes[category].found || rise > routes[category].rise) {
                    routes[category] = {start, dir, rise, true};
                }
            }
        }
    }
    return routes;
}

} // namespace

TEST_CASE("procedural LOD0: a settled capsule walks uphill across cells, diagonals and tiles") {
    physics::PhysicsWorld world;
    add_generated_tiles(world, -6, 5);

    const auto routes = find_routes(world);
    for (int category = 0; category < 6; ++category) {
        const Route& route = routes[category];
        INFO("route category=" << category);
        REQUIRE(route.found); // scanning must find a genuinely rising, walkable fixture
        physics::RayHit initial;
        REQUIRE(support(world, route.start.x, route.start.z, initial));
        gameplay::CharacterConfig config{};
        Character character;
        character.spawn(world,
                        config,
                        {route.start.x,
                         initial.point.y + config.half_height + config.radius + 2.0f,
                         route.start.z});
        character.tick_n(idle(), 120); // fall onto generated terrain; do not seed grounded state
        REQUIRE(character.state.grounded);
        REQUIRE_FALSE(character.overlapping());
        const auto settled = character.state.position;
        if (category == 0) {
            // Negative control: the public penetration query must actually detect a capsule
            // deliberately lowered into this generated heightfield.
            physics::QueryFilter static_filter;
            static_filter.dynamics = false;
            physics::PenetrationHit buried;
            REQUIRE(world.penetration(gameplay::character_shape(config),
                                      {settled.x, settled.y - 0.5f, settled.z},
                                      core::quat_identity(),
                                      buried,
                                      static_filter));
            REQUIRE(buried.depth > 0.10f);
        }

        int longest_airborne = 0;
        int airborne = 0;
        float worst_feet_below_axis = 0.0f;
        float min_progress = 0.0f;
        for (int tick = 0; tick < 80; ++tick) {
            character.tick(walk(route.direction.x, -route.direction.z));
            physics::RayHit hit;
            REQUIRE(support(world, character.state.position.x, character.state.position.z, hit));
            const float gap = character.feet_y() - hit.point.y;
            worst_feet_below_axis = std::min(worst_feet_below_axis, gap);
            airborne = character.state.grounded ? 0 : airborne + 1;
            longest_airborne = std::max(longest_airborne, airborne);
            const auto displacement = character.state.position - settled;
            min_progress = core::dot(displacement, route.direction);
            INFO("category=" << category << " start=(" << route.start.x << "," << route.start.z
                             << ") rise=" << route.rise << " tick=" << tick << " pose=("
                             << character.state.position.x << "," << character.state.position.y
                             << "," << character.state.position.z << ") feet-support=" << gap
                             << " grounded=" << character.state.grounded
                             << " stuck=" << character.stats.stuck
                             << " rejected=" << character.stats.step_rejected
                             << " phantom=" << character.stats.phantom_contacts);
            CHECK_FALSE(character.overlapping());
            CHECK(gap >= -0.10f); // same measured capsule-feet allowance as world-smoke
        }
        INFO("category=" << category << " start=(" << route.start.x << "," << route.start.z
                         << ") direction=(" << route.direction.x << "," << route.direction.z
                         << ") rise=" << route.rise << " progress=" << min_progress
                         << " worst feet-support=" << worst_feet_below_axis
                         << " longest airborne=" << longest_airborne
                         << " casts=" << character.stats.casts << " stuck=" << character.stats.stuck
                         << " rejected=" << character.stats.step_rejected);
        CHECK(character.stats.casts > 0);
        CHECK(character.stats.stuck == 0);
        CHECK(longest_airborne <= 8);
        CHECK(min_progress >= 4.0f);
        CHECK(character.state.position.y > settled.y + 0.10f);
        MESSAGE("route " << category << " start=(" << route.start.x << "," << route.start.z
                         << ") direction=(" << route.direction.x << "," << route.direction.z
                         << ") rise=" << route.rise << " progress=" << min_progress
                         << " worst_feet_gap=" << worst_feet_below_axis << " longest_airborne="
                         << longest_airborne << " stuck=" << character.stats.stuck
                         << " step_rejected=" << character.stats.step_rejected);
    }
}

TEST_CASE("procedural LOD0: westward uphill route sweep exposes movement stalls") {
    physics::PhysicsWorld world;
    add_generated_tiles(world, -6, -2);
    constexpr float d = 0.707106781f;
    const std::array<core::Vec3, 4> directions{{{1, 0, 0}, {-1, 0, 0}, {d, 0, d}, {-d, 0, -d}}};
    int checked = 0;
    for (int x = -328; x <= -176; x += 8) {
        for (int z = -24; z <= 24; z += 8) {
            for (const auto direction : directions) {
                const core::Vec3 start{static_cast<float>(x), 0, static_cast<float>(z)};
                float rise = 0;
                if (!walkable_route(world, start, direction, rise))
                    continue;
                ++checked;
                physics::RayHit initial;
                REQUIRE(support(world, start.x, start.z, initial));
                gameplay::CharacterConfig config{};
                Character character;
                character.spawn(world,
                                config,
                                {start.x,
                                 initial.point.y + config.half_height + config.radius + 2.0f,
                                 start.z});
                character.tick_n(idle(), 120);
                INFO("route start=(" << x << "," << z << ") dir=(" << direction.x << ","
                                     << direction.z << ") rise=" << rise);
                REQUIRE(character.state.grounded);
                const auto settled = character.state.position;
                int airborne = 0;
                int longest_airborne = 0;
                float worst_gap = 0;
                bool overlap = false;
                for (int tick = 0; tick < 80; ++tick) {
                    character.tick(walk(direction.x, -direction.z));
                    physics::RayHit hit;
                    REQUIRE(support(
                        world, character.state.position.x, character.state.position.z, hit));
                    worst_gap = std::min(worst_gap, character.feet_y() - hit.point.y);
                    overlap |= character.overlapping();
                    airborne = character.state.grounded ? 0 : airborne + 1;
                    longest_airborne = std::max(longest_airborne, airborne);
                }
                const float progress = core::dot(character.state.position - settled, direction);
                INFO("progress=" << progress << " worst_gap=" << worst_gap << " longest_airborne="
                                 << longest_airborne << " stuck=" << character.stats.stuck
                                 << " step_rejected=" << character.stats.step_rejected);
                REQUIRE_FALSE(overlap);
                REQUIRE(worst_gap >= -0.1f);
                REQUIRE(longest_airborne <= 8);
                REQUIRE(character.stats.stuck == 0);
                REQUIRE(progress >= 4.0f);
            }
        }
    }
    MESSAGE("westward route sweep checked " << checked << " walkable uphill routes");
    CHECK(checked >= 20);
}

TEST_CASE("procedural LOD0: sustained westward travel keeps support and capsule clearance") {
    physics::PhysicsWorld world;
    add_generated_tiles(world, -6, -2);
    for (float z : {-16.0f, 0.0f, 16.0f}) {
        physics::RayHit initial;
        REQUIRE(support(world, -120.0f, z, initial));
        gameplay::CharacterConfig config{};
        Character character;
        character.spawn(world,
                        config,
                        {-120.0f, initial.point.y + config.half_height + config.radius + 2.0f, z});
        character.tick_n(idle(), 120);
        REQUIRE(character.state.grounded);
        int walkable_stalls = 0;
        float window_x = character.state.position.x;
        for (int tick = 0; tick < 2700; ++tick) {
            character.tick(walk(-1.0f, 0.0f));
            physics::RayHit hit;
            REQUIRE(support(world, character.state.position.x, character.state.position.z, hit));
            const float gap = character.feet_y() - hit.point.y;
            INFO("z=" << z << " tick=" << tick << " x=" << character.state.position.x
                      << " y=" << character.state.position.y << " normal_y=" << hit.normal.y
                      << " feet_gap=" << gap << " stuck=" << character.stats.stuck);
            REQUIRE_FALSE(character.overlapping());
            REQUIRE(gap >= -0.10f);
            if (tick % 60 == 59) {
                const float progress = window_x - character.state.position.x;
                if (progress < 0.25f && hit.normal.y >= config.max_slope_cos) {
                    ++walkable_stalls;
                }
                window_x = character.state.position.x;
            }
        }
        MESSAGE("westward lane z=" << z << " end_x=" << character.state.position.x
                                   << " walkable_stalls=" << walkable_stalls
                                   << " stuck=" << character.stats.stuck
                                   << " step_rejected=" << character.stats.step_rejected);
        CHECK(character.stats.stuck == 0);
        CHECK(walkable_stalls == 0);
    }
}
