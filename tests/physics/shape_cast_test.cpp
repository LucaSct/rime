// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// m12.1 proofs: SHAPE CASTS — a convex shape swept along a line, answered by conservative
// advancement over GJK distance (src/scene_query.hpp), reached only through the public
// PhysicsWorld seam as the house pattern requires.
//
// The case that justifies the whole query is "a sphere too fat for the gap". A raycast is
// infinitely thin, so it threads any gap at all; that is exactly why a character controller built
// on rays walks through door frames it should have caught on. Every other test here is arithmetic
// that a swept-primitive routine would also satisfy — the gap test is the one that says why this
// query exists.
//
// The second load-bearing case is `initial_overlap`. "Touched after moving 0 m" and "started inside
// a wall" are the same number and opposite situations: the first means stop, the second means
// depenetrate first. A controller that confuses them freezes solid inside the geometry it is stuck
// in, so the two are asserted apart rather than together.
using namespace rime;

namespace {

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
                    physics::MotionType motion = physics::MotionType::Static,
                    core::Quat q = core::quat_identity()) {
    physics::BodyDesc d;
    d.motion = motion;
    d.shape = s;
    d.position = pos;
    d.orientation = q;
    return w.create_body(d);
}

physics::ShapeCast
cast_along(const physics::ShapeDesc& s, core::Vec3 origin, core::Vec3 dir, float distance) {
    physics::ShapeCast c;
    c.shape = s;
    c.origin = origin;
    c.direction = dir;
    c.max_distance = distance;
    return c;
}

} // namespace

TEST_CASE("m12.1 shape cast: a swept sphere stops at the surface, not at the centre") {
    physics::PhysicsWorld w;
    const physics::BodyId b = add(w, box({1.0f, 1.0f, 1.0f}), {0.0f, 0.0f, 0.0f});

    // A 0.5 m sphere starting 5 m out and sweeping in. Surfaces meet when the centre reaches
    // x = -(1.0 + 0.5), so it travels 5 - 1.5 = 3.5 m.
    physics::ShapeHit hit;
    REQUIRE(w.shape_cast(cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f),
                         hit));
    CHECK(hit.body.index == b.index);
    CHECK(hit.distance == doctest::Approx(3.5f).epsilon(0.01));
    CHECK_FALSE(hit.initial_overlap);

    SUBCASE("the normal is the target's outward face, pointing back at the caster") {
        CHECK(hit.normal.x == doctest::Approx(-1.0f).epsilon(0.02));
        CHECK(std::fabs(hit.normal.y) < 0.05f);
        CHECK(std::fabs(hit.normal.z) < 0.05f);
    }

    SUBCASE("the witness point is on the target's surface") {
        CHECK(hit.point.x == doctest::Approx(-1.0f).epsilon(0.02));
    }

    SUBCASE("a direction need not be unit — the distance is still world units") {
        physics::ShapeHit scaled;
        REQUIRE(w.shape_cast(
            cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {17.0f, 0.0f, 0.0f}, 10.0f), scaled));
        CHECK(scaled.distance == doctest::Approx(hit.distance));
    }
}

TEST_CASE("m12.1 shape cast: the fat-body case a raycast gets wrong") {
    // Two walls with a 1.0 m gap between their facing surfaces, centred on the sweep line. A ray
    // down the middle sails through; a sphere only fits if it is narrower than the gap.
    physics::PhysicsWorld w;
    // Each box is 4 m deep in z (half-extent 2.0) and centred at z = ∓2.5, so their facing
    // surfaces sit at z = -2.5 + 2.0 = -0.5 and z = +2.5 - 2.0 = +0.5: a 1.0 m gap around z = 0.
    // The sweep runs along +x at z = 0, straight down the middle of it.
    add(w, box({0.2f, 2.0f, 2.0f}), {0.0f, 0.0f, -2.5f});
    add(w, box({0.2f, 2.0f, 2.0f}), {0.0f, 0.0f, 2.5f});
    physics::RayHit ray_hit;
    const bool ray_blocked =
        w.raycast(physics::Ray{{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f}, ray_hit);
    CHECK_FALSE(ray_blocked); // the thin probe threads the gap, as it must

    SUBCASE("a sphere narrower than the gap passes too") {
        physics::ShapeHit hit;
        CHECK_FALSE(w.shape_cast(
            cast_along(sphere(0.45f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f), hit));
    }

    SUBCASE("a sphere wider than the gap is CAUGHT — the whole reason this query exists") {
        physics::ShapeHit hit;
        REQUIRE(w.shape_cast(
            cast_along(sphere(0.7f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f), hit));
        CHECK_FALSE(hit.initial_overlap);
        // It catches on the leading edge of a wall, i.e. around x = -0.2 minus its own radius.
        CHECK(hit.distance < 5.0f);
        CHECK(hit.distance > 3.0f);
    }
}

TEST_CASE("m12.1 shape cast: starting inside is NOT the same as stopping at zero") {
    physics::PhysicsWorld w;
    add(w, box({1.0f, 1.0f, 1.0f}), {0.0f, 0.0f, 0.0f});

    SUBCASE("a cast begun inside the body reports initial_overlap at distance 0") {
        physics::ShapeHit hit;
        REQUIRE(w.shape_cast(
            cast_along(sphere(0.25f), {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f), hit));
        CHECK(hit.initial_overlap);
        CHECK(hit.distance == doctest::Approx(0.0f));
    }

    SUBCASE("a cast begun exactly touching does NOT claim an overlap") {
        // Centre at x = -1.5 with radius 0.5: the surfaces are flush, the gap is 0, nothing
        // interpenetrates. This is the distinction the flag exists to preserve.
        physics::ShapeHit hit;
        REQUIRE(w.shape_cast(
            cast_along(sphere(0.5f), {-1.5f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f), hit));
        CHECK(hit.distance == doctest::Approx(0.0f).epsilon(0.01));
        CHECK_FALSE(hit.initial_overlap);
    }

    SUBCASE("an overlap wins over a nearer-in-time touch further along the sweep") {
        // A second wall the sweep would reach at ~3 m. The body we are ALREADY inside must be the
        // one reported, because a caller that must depenetrate needs to hear about that one.
        add(w, box({0.5f, 2.0f, 2.0f}), {4.0f, 0.0f, 0.0f});
        physics::ShapeHit hit;
        REQUIRE(w.shape_cast(
            cast_along(sphere(0.25f), {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 8.0f), hit));
        CHECK(hit.initial_overlap);
        CHECK(hit.distance == doctest::Approx(0.0f));
    }
}

TEST_CASE("m12.1 shape cast: a clean sweep misses, and the bound is respected") {
    physics::PhysicsWorld w;
    add(w, box({1.0f, 1.0f, 1.0f}), {0.0f, 0.0f, 0.0f});

    SUBCASE("nothing in the way") {
        physics::ShapeHit hit;
        CHECK_FALSE(w.shape_cast(
            cast_along(sphere(0.5f), {-5.0f, 10.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f), hit));
    }

    SUBCASE("the target is real but beyond max_distance") {
        physics::ShapeHit hit;
        // The touch is at 3.5 m; a 3.0 m sweep must not find it.
        CHECK_FALSE(w.shape_cast(
            cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 3.0f), hit));
    }

    SUBCASE("sweeping AWAY from the target misses") {
        physics::ShapeHit hit;
        CHECK_FALSE(w.shape_cast(
            cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, 10.0f), hit));
    }
}

TEST_CASE("m12.1 shape cast: preconditions are refused, not silently reinterpreted") {
    physics::PhysicsWorld w;
    add(w, box({1.0f, 1.0f, 1.0f}), {0.0f, 0.0f, 0.0f});
    physics::ShapeHit hit;

    SUBCASE("an unbounded sweep is rejected — its swept AABB would be the whole world") {
        // The one place this differs from Ray, which defaults to an effectively infinite distance.
        // Silently scanning every body in the world is the failure being prevented.
        auto c = cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f);
        c.max_distance = std::numeric_limits<float>::infinity();
        CHECK_FALSE(w.shape_cast(c, hit));
    }

    SUBCASE("a zero or negative distance is rejected") {
        CHECK_FALSE(w.shape_cast(
            cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.0f), hit));
        CHECK_FALSE(w.shape_cast(
            cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, -1.0f), hit));
    }

    SUBCASE("a zero-length direction is rejected") {
        CHECK_FALSE(w.shape_cast(
            cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 10.0f), hit));
    }
}

TEST_CASE("m12.1 shape cast: a capsule sweeps, and its radius counts as much as a sphere's") {
    physics::PhysicsWorld w;
    // A floor slab; a standing capsule dropped onto it. The capsule's local Y half-height is 0.6
    // and its radius 0.4, so its lowest point is 1.0 below its origin.
    add(w, box({5.0f, 0.5f, 5.0f}), {0.0f, -0.5f, 0.0f}); // top surface at y = 0

    physics::ShapeHit hit;
    REQUIRE(w.shape_cast(
        cast_along(capsule(0.4f, 0.6f), {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f), hit));
    // The bottom cap starts at y = 2.0 and the floor is at y = 0, so it falls 2.0 m.
    CHECK(hit.distance == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(hit.normal.y ==
          doctest::Approx(1.0f).epsilon(0.02)); // floor's up face, back at the caster
    CHECK_FALSE(hit.initial_overlap);
}

TEST_CASE("m12.1 shape cast: the filter gates whole trees, as raycast's does") {
    physics::PhysicsWorld w;
    add(w, box({1.0f, 1.0f, 1.0f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Dynamic);

    physics::ShapeHit hit;
    const auto c = cast_along(sphere(0.5f), {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f);

    CHECK(w.shape_cast(c, hit, physics::QueryFilter{true, true}));
    CHECK(w.shape_cast(c, hit, physics::QueryFilter{false, true})); // dynamics only: still found
    CHECK_FALSE(w.shape_cast(c, hit, physics::QueryFilter{true, false})); // statics only: gone
}

TEST_CASE("m12.1 shape cast: the nearest of several bodies wins") {
    physics::PhysicsWorld w;
    const physics::BodyId near = add(w, box({0.5f, 2.0f, 2.0f}), {-1.0f, 0.0f, 0.0f});
    add(w, box({0.5f, 2.0f, 2.0f}), {2.0f, 0.0f, 0.0f});

    physics::ShapeHit hit;
    REQUIRE(w.shape_cast(cast_along(sphere(0.25f), {-6.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 12.0f),
                         hit));
    CHECK(hit.body.index == near.index);
    // Near box's leading face is at x = -1.5; a 0.25 sphere from x = -6 travels 4.25 m.
    CHECK(hit.distance == doctest::Approx(4.25f).epsilon(0.01));
}

TEST_CASE("m12.1 shape cast: a compound reports WHICH child was caught") {
    physics::PhysicsWorld w;
    // Two children side by side in z; the sweep runs along +x offset toward the second, so the
    // answer is not the trivially-first child.
    std::vector<physics::CompoundChildDesc> children(2);
    children[0].shape = box({0.4f, 0.4f, 0.4f});
    children[0].position = {0.0f, 0.0f, -1.0f};
    children[1].shape = box({0.4f, 0.4f, 0.4f});
    children[1].position = {0.0f, 0.0f, 1.0f};
    const physics::CompoundId cid = w.register_compound(physics::CompoundDesc{children});
    REQUIRE(cid.is_valid());

    physics::ShapeDesc cs;
    cs.type = physics::ShapeType::Compound;
    cs.compound = cid;
    add(w, cs, {0.0f, 0.0f, 0.0f});

    physics::ShapeHit hit;
    REQUIRE(w.shape_cast(cast_along(sphere(0.2f), {-5.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 10.0f),
                         hit));
    CHECK(hit.child == 1); // the +z child, the one actually in the way
    CHECK_FALSE(hit.initial_overlap);
}

TEST_CASE("m12.1 shape cast: the same cast twice gives bit-identical answers") {
    // Determinism is not decoration here: the character controller m12.2 builds on this is a PURE
    // function whose replay under prediction (m12.4) must reproduce the original step exactly. A
    // query that wobbled would put the wobble inside reconciliation, where it would be diagnosed as
    // a network bug for a week.
    physics::PhysicsWorld w;
    add(w, box({1.0f, 1.0f, 1.0f}), {0.0f, 0.0f, 0.0f});
    add(w, sphere(0.7f), {1.0f, 1.2f, 0.3f});
    add(w, capsule(0.3f, 0.5f), {-2.0f, 0.4f, 0.9f});

    const auto c =
        cast_along(capsule(0.25f, 0.4f), {-6.0f, 0.3f, 0.2f}, {1.0f, 0.05f, 0.1f}, 12.0f);
    physics::ShapeHit a;
    physics::ShapeHit b;
    REQUIRE(w.shape_cast(c, a));
    REQUIRE(w.shape_cast(c, b));
    CHECK(a.distance == b.distance); // exact equality, not Approx
    CHECK(a.point.x == b.point.x);
    CHECK(a.point.y == b.point.y);
    CHECK(a.point.z == b.point.z);
    CHECK(a.normal.x == b.normal.x);
    CHECK(a.body.index == b.body.index);
    CHECK(a.child == b.child);
    CHECK(a.initial_overlap == b.initial_overlap);
}

TEST_CASE("m12.1 shape cast: a sweep that must not tunnel, at speed") {
    // The tunnelling case in query form: a small shape crossing a thin wall in one long step. A
    // discrete overlap test at the start and end positions sees nothing at either end; conservative
    // advancement cannot miss it, because every step it takes is bounded by the distance remaining.
    physics::PhysicsWorld w;
    add(w, box({0.05f, 5.0f, 5.0f}), {0.0f, 0.0f, 0.0f}); // a 10 cm thin wall

    physics::ShapeHit hit;
    REQUIRE(w.shape_cast(
        cast_along(sphere(0.05f), {-50.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f), hit));
    CHECK(hit.distance == doctest::Approx(49.9f).epsilon(0.001));
    CHECK(hit.normal.x == doctest::Approx(-1.0f).epsilon(0.02));
}
