// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// m12.2 proofs: PhysicsWorld::penetration() — the depenetration query.
//
// shape_cast can tell a caller it started inside something; it cannot say by how much or which way
// out, because an overlapping GJK terminates on a simplex and carries no witness points. Getting an
// axis out of that simplex is EPA's job, and this query is its public face. A character controller
// without it freezes the first time a crate is solver-pushed into it.
//
// Everything here is ANALYTIC: overlaps of axis-aligned boxes and concentric-axis spheres have a
// closed-form minimum translation, so the assertions compare against arithmetic rather than against
// last run's numbers. Public seam only — EPA lives under src/ and stays there.
using namespace rime;

namespace {

physics::ShapeDesc box(core::Vec3 half) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.half_extents = half;
    return s;
}

physics::ShapeDesc sphere(float r) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Sphere;
    s.radius = r;
    return s;
}

physics::ShapeDesc capsule(float r, float hh) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Capsule;
    s.radius = r;
    s.half_height = hh;
    return s;
}

physics::ShapeDesc compound_shape(physics::CompoundId id) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Compound;
    s.compound = id;
    return s;
}

physics::BodyId add(physics::PhysicsWorld& w,
                    const physics::ShapeDesc& s,
                    core::Vec3 pos,
                    physics::MotionType motion = physics::MotionType::Static,
                    core::Quat q = core::quat_identity()) {
    physics::BodyDesc d;
    d.motion = motion;
    d.shape = s;
    d.position = pos;
    d.orientation = q;
    return w.create_body(d);
}

} // namespace

TEST_CASE("m12.2 penetration: axis-aligned box overlap reports the analytic MTD") {
    // Two unit cubes (half-extent 0.5) whose centres are 0.8 m apart on X. They share
    // 1.0 - 0.8 = 0.2 m along X and a full 1.0 m on Y and Z, so the minimum translation is the X
    // one and the query box gets out by moving -X.
    physics::PhysicsWorld w;
    const physics::BodyId wall = add(w, box({0.5f, 0.5f, 0.5f}), {0.8f, 0.0f, 0.0f});

    physics::PenetrationHit hit;
    REQUIRE(w.penetration(box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}, core::quat_identity(), hit));
    CHECK(hit.body.index == wall.index);
    CHECK(hit.depth == doctest::Approx(0.2f).epsilon(0.02));
    CHECK(hit.normal.x == doctest::Approx(-1.0f).epsilon(0.01));
    CHECK(std::fabs(hit.normal.y) < 1e-2f);
    CHECK(std::fabs(hit.normal.z) < 1e-2f);

    // The reported vector really does separate: stepping the query shape by depth (plus a hair)
    // along `normal` leaves it clear of everything.
    const core::Vec3 pushed = core::Vec3{0.0f, 0.0f, 0.0f} + hit.normal * (hit.depth + 1e-3f);
    physics::PenetrationHit after;
    CHECK_FALSE(w.penetration(box({0.5f, 0.5f, 0.5f}), pushed, core::quat_identity(), after));
}

TEST_CASE("m12.2 penetration: sphere overlap reports centre-line direction and depth") {
    // Two unit spheres 1.5 m apart: they overlap by 2.0 - 1.5 = 0.5 m along the centre line.
    // EPA approximates a curved surface by an inscribed polytope, so its depth is a slight
    // UNDER-estimate — hence the looser tolerance here than for the flat-faced box case.
    physics::PhysicsWorld w;
    add(w, sphere(1.0f), {1.5f, 0.0f, 0.0f});

    physics::PenetrationHit hit;
    REQUIRE(w.penetration(sphere(1.0f), {0.0f, 0.0f, 0.0f}, core::quat_identity(), hit));
    CHECK(hit.depth == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(hit.normal.x == doctest::Approx(-1.0f).epsilon(0.02));
}

TEST_CASE("m12.2 penetration: a capsule sunk into a floor is pushed straight up") {
    // The controller's own case: a capsule (r = 0.4, cylinder half-height 0.5, so its lowest point
    // is 0.9 below its origin) with its origin at y = 0.8 over a floor whose top face is y = 0. It
    // is 0.1 m under, and the way out is +Y.
    physics::PhysicsWorld w;
    add(w, box({10.0f, 0.5f, 10.0f}), {0.0f, -0.5f, 0.0f});

    physics::PenetrationHit hit;
    REQUIRE(w.penetration(capsule(0.4f, 0.5f), {0.0f, 0.8f, 0.0f}, core::quat_identity(), hit));
    CHECK(hit.depth == doctest::Approx(0.1f).epsilon(0.05));
    CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("m12.2 penetration: separated poses report nothing") {
    physics::PhysicsWorld w;
    add(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f});

    physics::PenetrationHit hit;
    // Clearly apart.
    CHECK_FALSE(
        w.penetration(box({0.5f, 0.5f, 0.5f}), {5.0f, 0.0f, 0.0f}, core::quat_identity(), hit));
    // Just apart — a 1 cm gap, well inside the fat broadphase box, so this exercises the exact
    // test rejecting a candidate the broadphase admitted rather than the broadphase culling it.
    CHECK_FALSE(
        w.penetration(box({0.5f, 0.5f, 0.5f}), {1.01f, 0.0f, 0.0f}, core::quat_identity(), hit));
    // An empty world has nothing to be inside of.
    physics::PhysicsWorld empty;
    CHECK_FALSE(
        empty.penetration(box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}, core::quat_identity(), hit));
}

TEST_CASE("m12.2 penetration: the DEEPEST overlap wins, not the first one found") {
    // Wedged between two walls, 0.1 m into one and 0.3 m into the other. Resolving the deeper
    // violation first is what makes repeated recovery converge.
    physics::PhysicsWorld w;
    add(w, box({0.5f, 0.5f, 0.5f}), {0.9f, 0.0f, 0.0f}); // shallow: 0.1 m along +X
    const physics::BodyId deep =
        add(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, -0.7f}); // deep: 0.3 m along -Z

    physics::PenetrationHit hit;
    REQUIRE(w.penetration(box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}, core::quat_identity(), hit));
    CHECK(hit.body.index == deep.index);
    CHECK(hit.depth == doctest::Approx(0.3f).epsilon(0.02));
    CHECK(hit.normal.z == doctest::Approx(1.0f).epsilon(0.01)); // out along +Z
}

TEST_CASE("m12.2 penetration: the filter selects by motion class and honours exclude") {
    physics::PhysicsWorld w;
    const physics::BodyId statik = add(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, -0.7f});
    const physics::BodyId kinematic =
        add(w, box({0.5f, 0.5f, 0.5f}), {0.9f, 0.0f, 0.0f}, physics::MotionType::Kinematic);

    const physics::ShapeDesc query = box({0.5f, 0.5f, 0.5f});
    const core::Vec3 at{0.0f, 0.0f, 0.0f};

    physics::PenetrationHit hit;
    REQUIRE(w.penetration(query, at, core::quat_identity(), hit));
    CHECK(hit.body.index == statik.index); // the deeper of the two

    // Statics only: still the static one.
    REQUIRE(
        w.penetration(query, at, core::quat_identity(), hit, physics::QueryFilter{true, false}));
    CHECK(hit.body.index == statik.index);

    // Dynamics only: the kinematic box is in the dynamics tree, so it is what is left.
    REQUIRE(
        w.penetration(query, at, core::quat_identity(), hit, physics::QueryFilter{false, true}));
    CHECK(hit.body.index == kinematic.index);

    // exclude beats depth: naming the deeper body leaves the shallower one.
    physics::QueryFilter skip_deep;
    skip_deep.exclude = statik;
    REQUIRE(w.penetration(query, at, core::quat_identity(), hit, skip_deep));
    CHECK(hit.body.index == kinematic.index);

    // Excluding everything reports nothing at all.
    CHECK_FALSE(
        w.penetration(query, at, core::quat_identity(), hit, physics::QueryFilter{false, false}));
}

TEST_CASE("m12.2 penetration: a compound body names the child the shape is buried in") {
    // Two feet at x = ±1, each a 0.5-cube. The query box sits inside the +X foot only, so `child`
    // must name child 1 — the M8.3 convention that lets a caller say WHICH destructible part.
    physics::PhysicsWorld w;
    const physics::CompoundChildDesc kids[2] = {
        physics::CompoundChildDesc{box({0.5f, 0.5f, 0.5f}), {-1.0f, 0.0f, 0.0f}, {}},
        physics::CompoundChildDesc{box({0.5f, 0.5f, 0.5f}), {1.0f, 0.0f, 0.0f}, {}}};
    const physics::CompoundId id = w.register_compound(physics::CompoundDesc{kids});
    REQUIRE(id.is_valid());
    const physics::BodyId body = add(w, compound_shape(id), {0.0f, 0.0f, 0.0f});

    physics::PenetrationHit hit;
    REQUIRE(w.penetration(box({0.2f, 0.2f, 0.2f}), {1.0f, 0.0f, 0.0f}, core::quat_identity(), hit));
    CHECK(hit.body.index == body.index);
    CHECK(hit.child == 1);

    // A COMPOUND QUERY SHAPE is refused — it is not convex, so GJK/EPA have no support function to
    // ask. Same refusal shape_cast makes, for the same reason.
    CHECK_FALSE(w.penetration(compound_shape(id), {0.0f, 0.0f, 0.0f}, core::quat_identity(), hit));
}

TEST_CASE("m12.2 penetration: the query is deterministic and leaves the world untouched") {
    // Const in the type system is a claim; this is the check. Same call twice, and the world's
    // motion-state fingerprint before and after.
    physics::PhysicsWorld w;
    add(w, box({0.5f, 0.5f, 0.5f}), {0.8f, 0.0f, 0.0f});
    add(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, -0.7f});

    const std::uint64_t before = w.world_hash();

    physics::PenetrationHit a;
    physics::PenetrationHit b;
    REQUIRE(w.penetration(box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}, core::quat_identity(), a));
    REQUIRE(w.penetration(box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}, core::quat_identity(), b));

    CHECK(a.body == b.body);
    CHECK(a.child == b.child);
    CHECK(a.depth == b.depth); // bit-identical, not Approx — the query is a pure function
    CHECK(a.normal.x == b.normal.x);
    CHECK(a.normal.y == b.normal.y);
    CHECK(a.normal.z == b.normal.z);
    CHECK(w.world_hash() == before);
}
