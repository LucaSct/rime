// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.3 proofs: the narrowphase turns broadphase pairs into exact contact manifolds. Every check is
// analytic (closed-form penetration/normal for hand-placed shapes) and drives the *public*
// PhysicsWorld::compute_contacts seam — the algorithm internals (GJK/EPA/clipping under src/) are
// exercised only through it, the same way broadphase_test.cpp tests the AABB tree through
// compute_pairs. All pure-CPU; no GPU, no solver (contact *response* is M7.4).
using namespace rime;

namespace {

physics::ShapeDesc sphere(float r) {
    return physics::ShapeDesc{physics::ShapeType::Sphere, r, {}, 0.0f};
}

physics::ShapeDesc box(core::Vec3 half) {
    return physics::ShapeDesc{physics::ShapeType::Box, 0.0f, half, 0.0f};
}

physics::ShapeDesc capsule(float r, float half_height) {
    return physics::ShapeDesc{physics::ShapeType::Capsule, r, {}, half_height};
}

physics::BodyId add_body(physics::PhysicsWorld& w,
                         const physics::ShapeDesc& shape,
                         core::Vec3 pos,
                         physics::MotionType motion = physics::MotionType::Dynamic,
                         core::Quat q = core::quat_identity()) {
    physics::BodyDesc d;
    d.motion = motion;
    d.shape = shape;
    d.position = pos;
    d.orientation = q;
    return w.create_body(d);
}

} // namespace

TEST_CASE("sphere-sphere: analytic depth and normal, and clean separation") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    // Centres 0.8 m apart, radii 0.5 each: they overlap by (0.5 + 0.5) - 0.8 = 0.2 m along +X.
    (void)add_body(w, sphere(0.5f), {0.0f, 0.0f, 0.0f}); // slot 0 => manifold body a
    (void)add_body(w, sphere(0.5f), {0.8f, 0.0f, 0.0f}); // slot 1 => body b (a -> b is +X)

    std::vector<physics::Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    CHECK(ms[0].count == 1);
    CHECK(ms[0].normal.x == doctest::Approx(1.0f));
    CHECK(ms[0].normal.y == doctest::Approx(0.0f));
    CHECK(ms[0].normal.z == doctest::Approx(0.0f));
    CHECK(ms[0].points[0].penetration == doctest::Approx(0.2f));
    CHECK(ms[0].points[0].position.x == doctest::Approx(0.4f)); // midway between the surfaces

    physics::PhysicsWorld apart;
    (void)add_body(apart, sphere(0.5f), {0.0f, 0.0f, 0.0f});
    (void)add_body(apart, sphere(0.5f), {2.0f, 0.0f, 0.0f}); // 1 m gap > fat margin
    apart.compute_contacts(ms);
    CHECK(ms.empty());
}

TEST_CASE("sphere-box: resting on a face gives one point with the face normal and depth") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    (void)add_body(w,
                   box({0.5f, 0.5f, 0.5f}),
                   {0.0f, 0.0f, 0.0f},
                   physics::MotionType::Static);         // top face at y = 0.5, body a
    (void)add_body(w, sphere(0.5f), {0.0f, 0.9f, 0.0f}); // bottom at 0.4 => overlap 0.1, body b

    std::vector<physics::Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    CHECK(ms[0].count == 1);
    CHECK(ms[0].normal.x == doctest::Approx(0.0f));
    CHECK(ms[0].normal.y == doctest::Approx(1.0f)); // box (a) -> sphere (b) is +Y
    CHECK(ms[0].normal.z == doctest::Approx(0.0f));
    CHECK(ms[0].points[0].penetration == doctest::Approx(0.1f));
}

TEST_CASE("box-box: a resting face contact yields a four-point manifold") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    // A smaller box resting on a larger one: the incident face sits strictly inside the reference
    // face, so all four clipped corners survive unambiguously (no coincident-edge fp lottery).
    (void)add_body(w,
                   box({0.5f, 0.5f, 0.5f}),
                   {0.0f, 0.0f, 0.0f},
                   physics::MotionType::Static);                    // top at y = 0.5, body a
    (void)add_body(w, box({0.4f, 0.4f, 0.4f}), {0.0f, 0.8f, 0.0f}); // bottom at 0.4 => overlap 0.1

    std::vector<physics::Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    const physics::Manifold& m = ms[0];
    CHECK(m.count == 4);
    CHECK(m.normal.y == doctest::Approx(1.0f)); // lower (a) -> upper (b)

    float min_x = 1e9f;
    float max_x = -1e9f;
    float min_z = 1e9f;
    float max_z = -1e9f;
    for (int i = 0; i < m.count; ++i) {
        CHECK(m.points[i].penetration == doctest::Approx(0.1f).epsilon(0.02));
        min_x = std::min(min_x, m.points[i].position.x);
        max_x = std::max(max_x, m.points[i].position.x);
        min_z = std::min(min_z, m.points[i].position.z);
        max_z = std::max(max_z, m.points[i].position.z);
    }
    // The four points are the corners of the upper box's 0.8 x 0.8 bottom face.
    CHECK(min_x == doctest::Approx(-0.4f));
    CHECK(max_x == doctest::Approx(0.4f));
    CHECK(min_z == doctest::Approx(-0.4f));
    CHECK(max_z == doctest::Approx(0.4f));
}

TEST_CASE("box-box: a deep lateral overlap selects the minimum-translation axis") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    // Overlaps: x = 0.3 (min), y = 0.8, z = 0.8 -> EPA must separate along X by 0.3.
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}); // x in [-0.5, 0.5], body a
    (void)add_body(w, box({0.5f, 0.4f, 0.4f}), {0.7f, 0.0f, 0.0f}); // x in [0.2, 1.2], body b

    std::vector<physics::Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    const physics::Manifold& m = ms[0];
    CHECK(m.count >= 1);
    CHECK(m.normal.x == doctest::Approx(1.0f)); // a -> b is +X
    CHECK(m.normal.y == doctest::Approx(0.0f));
    CHECK(m.normal.z == doctest::Approx(0.0f));
    for (int i = 0; i < m.count; ++i) {
        CHECK(m.points[i].penetration == doctest::Approx(0.3f).epsilon(0.02));
    }
}

TEST_CASE("sphere-capsule fast path: closest point on the core segment") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    // Capsule core is the Y segment [-0.5, 0.5]; the sphere sits 0.5 m off the axis. Distance to
    // the core is 0.5, radii sum 0.7 -> overlap 0.2 along +X.
    (void)add_body(
        w, capsule(0.3f, 0.5f), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static); // body a
    (void)add_body(w, sphere(0.4f), {0.5f, 0.0f, 0.0f});                          // body b

    std::vector<physics::Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    CHECK(ms[0].count == 1);
    CHECK(ms[0].normal.x == doctest::Approx(1.0f)); // capsule (a) -> sphere (b)
    CHECK(ms[0].points[0].penetration == doctest::Approx(0.2f));
}

TEST_CASE("feature ids are frame-stable: the manifold cache warm-starts an unchanged contact") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    (void)add_body(w, box({0.4f, 0.4f, 0.4f}), {0.0f, 0.8f, 0.0f});

    std::vector<physics::Manifold> first;
    std::vector<physics::Manifold> second;

    w.compute_contacts(first);
    CHECK(w.contacts_warm_started_last() == 0); // the cache started empty
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].count == 4);

    w.compute_contacts(second);                 // identical geometry, one tick later
    CHECK(w.contacts_warm_started_last() == 4); // every point matched its cached feature id
    REQUIRE(second.size() == 1);

    std::set<std::uint32_t> ids_first;
    std::set<std::uint32_t> ids_second;
    for (int i = 0; i < first[0].count; ++i) {
        ids_first.insert(first[0].points[i].feature_id);
    }
    for (int i = 0; i < second[0].count; ++i) {
        ids_second.insert(second[0].points[i].feature_id);
    }
    CHECK(ids_first.size() == 4); // four distinct points
    CHECK(ids_first == ids_second);

    // Fling the upper box away (gravity up; a contact can only push, so nothing resists the
    // separation): once it separates, the pair is gone and the cache clears — nothing to
    // warm-start.
    w.set_gravity({0.0f, 100.0f, 0.0f});
    for (int k = 0; k < 30; ++k) {
        w.step(1.0f / 60.0f);
    }
    std::vector<physics::Manifold> gone;
    w.compute_contacts(gone);
    CHECK(gone.empty());
    CHECK(w.contacts_warm_started_last() == 0);
}

TEST_CASE("narrowphase is deterministic run to run, through stepping") {
    // A tight cluster of overlapping bodies, stepped a few ticks (the deep spawn overlaps far
    // outlast ten ticks of rate-limited NGS recovery, so manifolds persist). Same binary, same
    // inputs -> bit-for-bit the same manifolds (ids included).
    const auto run = []() {
        physics::PhysicsWorld w;
        w.set_gravity({0.0f, -9.81f, 0.0f});
        const core::Vec3 pos[5] = {{0.0f, 0.0f, 0.0f},
                                   {0.3f, 0.1f, 0.0f},
                                   {-0.2f, 0.2f, 0.1f},
                                   {0.1f, -0.2f, 0.2f},
                                   {0.0f, 0.15f, -0.25f}};
        for (int i = 0; i < 5; ++i) {
            add_body(w, i % 2 == 0 ? box({0.5f, 0.5f, 0.5f}) : sphere(0.5f), pos[i]);
        }
        for (int k = 0; k < 10; ++k) {
            w.step(1.0f / 120.0f);
        }
        std::vector<physics::Manifold> ms;
        w.compute_contacts(ms);
        return ms;
    };

    const std::vector<physics::Manifold> a = run();
    const std::vector<physics::Manifold> b = run();
    REQUIRE_FALSE(a.empty()); // guard against a vacuous comparison
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].a.index == b[i].a.index);
        CHECK(a[i].b.index == b[i].b.index);
        CHECK(a[i].count == b[i].count);
        CHECK(a[i].normal.x == doctest::Approx(b[i].normal.x));
        CHECK(a[i].normal.y == doctest::Approx(b[i].normal.y));
        CHECK(a[i].normal.z == doctest::Approx(b[i].normal.z));
        for (int j = 0; j < a[i].count; ++j) {
            CHECK(a[i].points[j].feature_id == b[i].points[j].feature_id);
            CHECK(a[i].points[j].penetration == doctest::Approx(b[i].points[j].penetration));
        }
    }
}

TEST_CASE("narrowphase survives create/destroy churn and its cache respects generations") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, -9.81f, 0.0f});

    // A platform-stable LCG (std <random> distributions are not bit-portable across stdlibs).
    std::uint32_t rng = 0x1234567u;
    const auto next01 = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    };
    const auto range = [&](float lo, float hi) { return lo + (hi - lo) * next01(); };

    std::vector<physics::BodyId> ids;
    std::vector<physics::Manifold> ms;
    for (int it = 0; it < 400; ++it) {
        if (ids.size() < 30 && (ids.empty() || next01() < 0.6f)) {
            const float which = next01();
            const physics::ShapeDesc s = which < 0.34f   ? sphere(0.4f)
                                         : which < 0.67f ? box({0.4f, 0.4f, 0.4f})
                                                         : capsule(0.3f, 0.4f);
            const physics::MotionType motion =
                next01() < 0.3f ? physics::MotionType::Static : physics::MotionType::Dynamic;
            // A tight cluster (<= 1.5 m) so shapes genuinely overlap and the narrowphase does work.
            ids.push_back(add_body(
                w, s, {range(-1.5f, 1.5f), range(-1.5f, 1.5f), range(-1.5f, 1.5f)}, motion));
        } else if (!ids.empty()) {
            rng = rng * 1664525u + 1013904223u;
            const std::size_t k = rng % ids.size();
            w.destroy_body(ids[k]);
            ids[k] = ids.back();
            ids.pop_back();
        }
        if (it % 5 == 0) {
            w.step(1.0f / 120.0f);
            w.compute_contacts(ms); // exercises GJK/EPA/clipping on a churning set (ASan lifetime)
            CHECK(w.validate_broadphase());
        }
    }

    // Targeted generation guard: a stable pair warm-starts, but a recycled slot must not inherit
    // the dead pair's cached contact (the guard that keeps M7.4's warm impulses honest under
    // churn).
    physics::PhysicsWorld g;
    g.set_gravity({0.0f, 0.0f, 0.0f});
    (void)add_body(g, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}); // slot 0, kept
    const physics::BodyId tmp = add_body(g, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.9f, 0.0f}); // slot 1
    g.compute_contacts(ms);
    g.compute_contacts(ms);
    CHECK(g.contacts_warm_started_last() > 0); // the (0,1) pair persisted
    g.destroy_body(tmp);
    (void)add_body(g, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.9f, 0.0f}); // recycles slot 1, bumped gen
    g.compute_contacts(ms);
    CHECK(g.contacts_warm_started_last() == 0); // stale entry rejected by the generation guard
}
