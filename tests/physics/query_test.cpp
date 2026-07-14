// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.7 proofs: SCENE QUERIES (raycast + overlap) and EXTERNAL IMPULSES. Pure CPU and structural, on
// the public PhysicsWorld seam — the broadphase BVH and the exact ray-vs-shape geometry
// (src/scene_query.hpp) are exercised only through raycast()/overlap_sphere(), as the house pattern
// keeps internals below the seam. These are the "askable, pushable world" the physics playground
// and all gameplay ride on.
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

physics::ShapeDesc box(core::Vec3 half) {
    return physics::ShapeDesc{physics::ShapeType::Box, 0.0f, half, 0.0f};
}

physics::ShapeDesc sphere(float r) {
    return physics::ShapeDesc{physics::ShapeType::Sphere, r, {}, 0.0f};
}

physics::ShapeDesc capsule(float r, float hh) {
    return physics::ShapeDesc{physics::ShapeType::Capsule, r, {}, hh};
}

physics::BodyId add(physics::PhysicsWorld& w,
                    const physics::ShapeDesc& s,
                    core::Vec3 pos,
                    physics::MotionType motion = physics::MotionType::Dynamic,
                    core::Quat q = core::quat_identity()) {
    physics::BodyDesc d;
    d.motion = motion;
    d.shape = s;
    d.position = pos;
    d.orientation = q;
    return w.create_body(d);
}

} // namespace

TEST_CASE("M7.7 query: raycast hits a sphere head-on with the right point, normal, distance") {
    physics::PhysicsWorld w;
    const physics::BodyId s = add(w, sphere(1.0f), {0.0f, 0.0f, 0.0f});

    physics::RayHit hit;
    REQUIRE(w.raycast(physics::Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, hit));
    CHECK(hit.body.index == s.index);
    CHECK(hit.distance == doctest::Approx(4.0f)); // origin z=5, sphere surface at z=1
    CHECK(hit.point.z == doctest::Approx(1.0f));
    CHECK(hit.normal.z == doctest::Approx(1.0f)); // outward, back toward the ray

    // A non-unit direction is normalized: the distance is still in world units.
    physics::RayHit hit2;
    REQUIRE(w.raycast(physics::Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -3.0f}}, hit2));
    CHECK(hit2.distance == doctest::Approx(4.0f));
}

TEST_CASE("M7.7 query: raycast hits an axis-aligned box face with the face normal") {
    physics::PhysicsWorld w;
    add(w, box({1.0f, 1.0f, 1.0f}), {0.0f, 0.0f, 0.0f});

    physics::RayHit hit;
    REQUIRE(w.raycast(physics::Ray{{5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, hit));
    CHECK(hit.distance == doctest::Approx(4.0f)); // +X face at x=1
    CHECK(hit.point.x == doctest::Approx(1.0f));
    CHECK(hit.normal.x == doctest::Approx(1.0f));
}

TEST_CASE("M7.7 query: raycast respects box orientation (OBB, not the AABB)") {
    // A tall thin box (half-Y = 2) rotated +90° about Z: its local Y axis now points along world X,
    // so the body reaches x = 2 (not the unrotated 0.5). A ray down −X must hit that rotated face
    // at x = 2 — proving the ray is tested against the oriented box, not a stale AABB.
    physics::PhysicsWorld w;
    const core::Quat rot = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.5707963f);
    add(w, box({0.5f, 2.0f, 0.5f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static, rot);

    physics::RayHit hit;
    REQUIRE(w.raycast(physics::Ray{{5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, hit));
    CHECK(hit.point.x == doctest::Approx(2.0f));
    CHECK(hit.normal.x == doctest::Approx(1.0f));
}

TEST_CASE("M7.7 query: raycast hits a capsule side and end-cap") {
    physics::PhysicsWorld w;
    add(w, capsule(0.5f, 1.0f), {0.0f, 0.0f, 0.0f}); // radius 0.5, cylinder along Y from -1..1

    // Side hit: fire along -X at y=0 (the cylinder's waist). Surface at x = 0.5.
    physics::RayHit side;
    REQUIRE(w.raycast(physics::Ray{{5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, side));
    CHECK(side.point.x == doctest::Approx(0.5f));
    CHECK(side.normal.x == doctest::Approx(1.0f));

    // Cap hit: fire straight down -Y along the axis. Top cap sphere centre (0,1,0) r=0.5 ⇒ y=1.5.
    physics::RayHit cap;
    REQUIRE(w.raycast(physics::Ray{{0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}}, cap));
    CHECK(cap.point.y == doctest::Approx(1.5f));
    CHECK(cap.normal.y == doctest::Approx(1.0f));
}

TEST_CASE("M7.7 query: a ray that misses reports no hit") {
    physics::PhysicsWorld w;
    add(w, sphere(1.0f), {0.0f, 0.0f, 0.0f});

    physics::RayHit hit;
    CHECK_FALSE(
        w.raycast(physics::Ray{{0.0f, 5.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, hit)); // passes above
    CHECK_FALSE(
        w.raycast(physics::Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}}, hit)); // points away
}

TEST_CASE("M7.7 query: raycast returns the NEAREST body and honours max_distance") {
    physics::PhysicsWorld w;
    const physics::BodyId near = add(w, sphere(0.5f), {0.0f, 0.0f, 2.0f});
    add(w, sphere(0.5f), {0.0f, 0.0f, -2.0f}); // farther along the ray

    physics::RayHit hit;
    REQUIRE(w.raycast(physics::Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, hit));
    CHECK(hit.body.index == near.index); // the near sphere, not the far one
    CHECK(hit.distance == doctest::Approx(2.5f));

    // Bound the cast short of even the near sphere: nothing is reported.
    physics::RayHit bounded;
    physics::Ray r{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}};
    r.max_distance = 1.0f;
    CHECK_FALSE(w.raycast(r, bounded));
}

TEST_CASE("M7.7 query: the filter selects by motion class") {
    physics::PhysicsWorld w;
    const physics::BodyId floor =
        add(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    const physics::BodyId ball = add(w, sphere(0.5f), {0.0f, 3.0f, 0.0f});

    // Straight down through the dynamic ball then the static floor.
    const physics::Ray down{{0.0f, 6.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};

    physics::RayHit any;
    REQUIRE(w.raycast(down, any));
    CHECK(any.body.index == ball.index); // nearest overall is the ball

    physics::RayHit only_static;
    REQUIRE(w.raycast(down, only_static, physics::QueryFilter{true, false}));
    CHECK(only_static.body.index == floor.index); // dynamics excluded ⇒ the floor

    physics::RayHit only_dynamic;
    REQUIRE(w.raycast(down, only_dynamic, physics::QueryFilter{false, true}));
    CHECK(only_dynamic.body.index == ball.index);
}

TEST_CASE("M7.7 query: overlap_sphere finds exactly the planted set, in canonical order") {
    physics::PhysicsWorld w;
    const physics::BodyId a = add(w, sphere(0.5f), {0.0f, 0.0f, 0.0f});
    const physics::BodyId b = add(w, box({0.5f, 0.5f, 0.5f}), {1.5f, 0.0f, 0.0f});
    add(w, sphere(0.5f), {50.0f, 0.0f, 0.0f}); // far away — must NOT be found

    std::vector<physics::BodyId> hits;
    w.overlap_sphere({0.5f, 0.0f, 0.0f}, 1.2f, hits); // reaches a (at 0) and b's face (at 1.0)
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].index == a.index); // canonical slot order (a created before b)
    CHECK(hits[1].index == b.index);
}

TEST_CASE("M7.7 impulse: a central impulse changes linear velocity by J/m, no spin") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f}); // isolate the impulse
    physics::BodyDesc d;
    d.shape = sphere(0.5f);
    d.mass = 2.0f;
    const physics::BodyId b = w.create_body(d);

    w.apply_central_impulse(b, {4.0f, 0.0f, 0.0f}); // J/m = 4/2 = 2 m/s
    physics::BodyState s;
    REQUIRE(w.get_body_state(b, s));
    CHECK(s.linear_velocity.x == doctest::Approx(2.0f));
    CHECK(s.angular_velocity.x == doctest::Approx(0.0f));
    CHECK(s.angular_velocity.y == doctest::Approx(0.0f));
    CHECK(s.angular_velocity.z == doctest::Approx(0.0f));
}

TEST_CASE("M7.7 impulse: an off-centre impulse induces spin about the expected axis") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::BodyId b = add(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f});

    // Push +X at a point above the COM (r = +Y): torque r×J = (0,1,0)×(1,0,0) = (0,0,-1) ⇒ spin −Z.
    w.apply_impulse(b, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    physics::BodyState s;
    REQUIRE(w.get_body_state(b, s));
    CHECK(s.linear_velocity.x > 0.0f);
    CHECK(s.angular_velocity.z < 0.0f);
}

TEST_CASE("M7.7 impulse: an impulse wakes a sleeping body; a static body ignores it") {
    physics::PhysicsWorld w;
    add(w, box({50.0f, 0.5f, 50.0f}), {0.0f, -0.5f, 0.0f}, physics::MotionType::Static); // floor
    const physics::BodyId b = add(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.5f, 0.0f});

    int steps = 0;
    for (; steps < 300 && !w.is_asleep(b); ++steps) {
        w.step(kDt);
    }
    REQUIRE(w.is_asleep(b));

    w.apply_central_impulse(b, {0.0f, 5.0f, 0.0f}); // an upward kick
    CHECK_FALSE(w.is_asleep(b));                    // woken

    // A static body has no inverse mass: an impulse is a no-op (it never moves under the sim).
    physics::BodyDesc sd;
    sd.motion = physics::MotionType::Static;
    sd.shape = box({1.0f, 1.0f, 1.0f});
    const physics::BodyId anchor = w.create_body(sd);
    w.apply_impulse(anchor, {100.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    physics::BodyState as;
    REQUIRE(w.get_body_state(anchor, as));
    CHECK(as.linear_velocity.x == doctest::Approx(0.0f));
    CHECK(as.angular_velocity.z == doctest::Approx(0.0f));
}
