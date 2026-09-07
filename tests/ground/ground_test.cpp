// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "rime/core/math/vec.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/ground/bind.hpp"
#include "rime/ground/derive.hpp"
#include "rime/physics/physics.hpp"
#include "rime/render/mesh.hpp"

// m17.8 proofs. The defect this module exists to end is not subtle and it is not hypothetical: in
// `99-the-block` one surface was DRAWN at half-extent 38, COLLIDED at 44 and GI-TRACED at 26 x 14,
// because three formulas in three files each derived it correctly from different numbers. So the
// proofs here are about AGREEMENT between derived artefacts rather than about any one of them being
// right in isolation — a test that checked `derive_collider` against the component's own fields
// would have passed happily in the old world too.
using namespace rime;

namespace {

struct Bounds {
    core::Vec3 min{std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    core::Vec3 max{std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
};

// The DRAWN extent, measured from the vertices the renderer would actually receive — deliberately
// not from `half_extents`, so the two are independent readings rather than one number twice.
[[nodiscard]] Bounds mesh_bounds(const render::CpuMesh& m) {
    Bounds b;
    for (const render::MeshVertex& v : m.vertices) {
        b.min.x = std::min(b.min.x, v.px);
        b.min.y = std::min(b.min.y, v.py);
        b.min.z = std::min(b.min.z, v.pz);
        b.max.x = std::max(b.max.x, v.px);
        b.max.y = std::max(b.max.y, v.py);
        b.max.z = std::max(b.max.z, v.pz);
    }
    return b;
}

} // namespace

TEST_CASE("m17.8: the drawn surface reaches exactly as far as the authored extent") {
    ground::GroundSurface s;
    s.half_x = 38.0f;
    s.half_z = 17.5f;
    s.cells_x = 19;
    s.cells_z = 7;

    const render::CpuMesh mesh = ground::derive_mesh(s);
    const Bounds b = mesh_bounds(mesh);

    // Corner-indexed rather than accumulated, so the far edge lands ON the extent rather than one
    // rounding error short of it — which for a 76 m surface at 19 cells is the difference between
    // a seam you can see and one you cannot.
    CHECK(b.min.x == doctest::Approx(-s.half_x));
    CHECK(b.max.x == doctest::Approx(+s.half_x));
    CHECK(b.min.z == doctest::Approx(-s.half_z));
    CHECK(b.max.z == doctest::Approx(+s.half_z));
    CHECK(b.min.y == doctest::Approx(0.0f));
    CHECK(b.max.y == doctest::Approx(0.0f));

    CHECK(mesh.vertices.size() == (19u + 1u) * (7u + 1u));
    CHECK(mesh.indices.size() == 19u * 7u * 6u);
}

TEST_CASE("m17.8: texture scale is a property of the surface, not of its size") {
    // The old street was a unit plane with `uv_tiles = 24` scaled to whatever span the block
    // happened to need, so making the street longer silently stretched its texture. Metre-based uvs
    // mean two grounds of very different sizes put the same number of metres under one repeat.
    ground::GroundSurface small;
    small.half_x = small.half_z = 8.0f;
    small.tile_metres = 2.0f;
    ground::GroundSurface large;
    large.half_x = large.half_z = 64.0f;
    large.tile_metres = 2.0f;

    const auto uv_span = [](const ground::GroundSurface& s) {
        const render::CpuMesh m = ground::derive_mesh(s);
        float lo = std::numeric_limits<float>::max();
        float hi = std::numeric_limits<float>::lowest();
        for (const render::MeshVertex& v : m.vertices) {
            lo = std::min(lo, v.u);
            hi = std::max(hi, v.u);
        }
        return hi - lo;
    };

    // Repeats across the whole surface = metres / tile_metres, so the RATIO of repeats to metres is
    // the invariant, and it is the same for both.
    CHECK(uv_span(small) == doctest::Approx(2.0f * small.half_x / small.tile_metres));
    CHECK(uv_span(large) == doctest::Approx(2.0f * large.half_x / large.tile_metres));
    CHECK(uv_span(large) / uv_span(small) == doctest::Approx(large.half_x / small.half_x));
}

TEST_CASE("m17.8: you can stand exactly where the ground is drawn, and nowhere else") {
    // THE PROOF THE MODULE EXISTS FOR, and it is deliberately a comparison between two INDEPENDENT
    // consumers — the vertices the renderer gets, and the body the physics query finds — rather
    // than between two copies of one number. In the old block this failed by six metres.
    ecs::World world;
    ground::register_ground_components(world);
    ecs::register_transform_components(world);
    physics::PhysicsWorld physics;

    ground::GroundSurface s;
    s.half_x = 38.0f;
    s.half_z = 22.0f;
    s.cells_x = s.cells_z = 8;
    s.thickness = 1.0f;

    core::Transform placement;
    placement.translation = {22.0f, 0.0f, 0.0f}; // off-origin, like the block's street
    const ecs::Entity e = world.spawn_with(ecs::LocalTransform{placement}, s);
    REQUIRE(e.is_valid());

    const ground::BindStats bound = ground::bind_ground(world, physics);
    CHECK(bound.bound == 1);
    CHECK(bound.scaled_refused == 0);

    const Bounds b = mesh_bounds(ground::derive_mesh(s));
    const auto stands_at = [&](float x, float z) {
        // Straight down from well above; a hit means there is ground to stand on here.
        physics::RayHit hit{};
        const physics::Ray ray{{x, 10.0f, z}, {0.0f, -1.0f, 0.0f}, 50.0f};
        return physics.raycast(ray, hit);
    };

    constexpr float kIn = 0.01f; // a centimetre inside / outside the drawn edge
    for (const float sign : {-1.0f, 1.0f}) {
        const float edge_x = placement.translation.x + (sign < 0 ? b.min.x : b.max.x);
        const float edge_z = placement.translation.z + (sign < 0 ? b.min.z : b.max.z);
        CHECK(stands_at(edge_x - sign * kIn, placement.translation.z));       // just inside: ground
        CHECK_FALSE(stands_at(edge_x + sign * kIn, placement.translation.z)); // just outside: none
        CHECK(stands_at(placement.translation.x, edge_z - sign * kIn));
        CHECK_FALSE(stands_at(placement.translation.x, edge_z + sign * kIn));
    }
}

TEST_CASE("m17.8: binding twice binds nothing, and a scaled surface is refused and counted") {
    ecs::World world;
    ground::register_ground_components(world);
    ecs::register_transform_components(world);
    physics::PhysicsWorld physics;

    core::Transform flat;
    (void)world.spawn_with(ecs::LocalTransform{flat}, ground::GroundSurface{});

    // A physics body cannot be scaled, so honouring this would mean folding the scale into the
    // shape — at which point the collider stops being derived from the authored numbers, which is
    // the whole defect. Refused, and COUNTED: a skip nobody counted reads exactly like no work.
    core::Transform scaled;
    scaled.scale = {2.0f, 1.0f, 2.0f};
    (void)world.spawn_with(ecs::LocalTransform{scaled}, ground::GroundSurface{});

    const ground::BindStats first = ground::bind_ground(world, physics);
    CHECK(first.bound == 1);
    CHECK(first.scaled_refused == 1);

    // Idempotent: the standing one is skipped, the refused one is refused again rather than
    // silently becoming valid on a later pass.
    const ground::BindStats second = ground::bind_ground(world, physics);
    CHECK(second.bound == 0);
    CHECK(second.scaled_refused == 1);
}

TEST_CASE("m17.8: a handle from a destroyed world does not count as bound") {
    // The editor's Play builds a BRAND-NEW PhysicsWorld while the entity keeps the `GroundBody`
    // written against the old one, and a BodyId is an index plus a generation — so a handle from a
    // world that no longer exists still answers `is_valid()`. Believing it would skip the bind and
    // leave the new world with no ground at all: a floor that is plainly drawn and falls through.
    ecs::World world;
    ground::register_ground_components(world);
    ecs::register_transform_components(world);

    ground::GroundSurface s;
    s.half_x = s.half_z = 12.0f;
    (void)world.spawn_with(ecs::LocalTransform{}, s);

    physics::PhysicsWorld first;
    CHECK(ground::bind_ground(world, first).bound == 1);
    CHECK(ground::bind_ground(world, first).bound == 0); // same world: still idempotent

    // Play pressed: a new world, and the component still holds the old world's handle.
    physics::PhysicsWorld second;
    CHECK(ground::bind_ground(world, second).bound == 1);

    // …and it really is standing in the NEW world, not merely re-stamped.
    physics::RayHit hit{};
    const physics::Ray down{{0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 50.0f};
    CHECK(second.raycast(down, hit));
}

TEST_CASE("m17.8: every peer derives the same ground, bit for bit") {
    // Level geometry is stood up independently by the server and by each client rather than
    // replicated, so two peers agreeing about where the floor is rests entirely on the derivation
    // being a pure function. Hashed over the raw vertex bytes, not compared field by field, so a
    // change to the vertex layout cannot quietly pass this.
    ground::GroundSurface s;
    s.half_x = 31.0f;
    s.half_z = 12.5f;
    s.cells_x = 13;
    s.cells_z = 9;
    s.tile_metres = 1.5f;

    const render::CpuMesh a = ground::derive_mesh(s);
    const render::CpuMesh b = ground::derive_mesh(s);
    REQUIRE(a.vertices.size() == b.vertices.size());
    CHECK(std::equal(reinterpret_cast<const std::byte*>(a.vertices.data()),
                     reinterpret_cast<const std::byte*>(a.vertices.data() + a.vertices.size()),
                     reinterpret_cast<const std::byte*>(b.vertices.data())));
    CHECK(a.indices == b.indices);

    const ground::GroundCollider ca = ground::derive_collider(s);
    const ground::GroundCollider cb = ground::derive_collider(s);
    CHECK(ca.shape.half_extents.x == cb.shape.half_extents.x);
    CHECK(ca.offset.y == cb.offset.y);

    // …and the collider's top face IS the drawn surface, which is what makes the raycast proof
    // above a statement about the seam rather than about one lucky constant.
    CHECK(ca.offset.y + ca.shape.half_extents.y == doctest::Approx(0.0f));
    CHECK(ca.shape.half_extents.x == doctest::Approx(ground::half_extents(s).x));
    CHECK(ca.shape.half_extents.z == doctest::Approx(ground::half_extents(s).z));
}
