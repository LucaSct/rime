// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <vector>

#include "rime/physics/physics.hpp"

using namespace rime;

namespace {

physics::ShapeDesc box(core::Vec3 half) {
    return {physics::ShapeType::Box, 0.0f, half, 0.0f};
}

physics::BodyId add(physics::PhysicsWorld& w,
                    physics::ShapeDesc shape,
                    core::Vec3 at,
                    physics::MotionType motion = physics::MotionType::Dynamic,
                    bool ccd = false,
                    core::Vec3 velocity = {}) {
    physics::BodyDesc d;
    d.shape = shape;
    d.position = at;
    d.motion = motion;
    d.ccd = ccd;
    d.linear_velocity = velocity;
    return w.create_body(d);
}

physics::ShapeDesc hull(physics::PhysicsWorld& w) {
    std::vector<core::Vec3> v;
    for (std::uint32_t i = 0; i < 8; ++i)
        v.push_back(
            {(i & 1u) ? 0.25f : -0.25f, (i & 2u) ? 0.25f : -0.25f, (i & 4u) ? 0.25f : -0.25f});
    const std::array<std::uint32_t, 6> counts{4, 4, 4, 4, 4, 4};
    const std::array<std::uint32_t, 24> faces{0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4,
                                              2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    physics::ShapeDesc s;
    s.type = physics::ShapeType::ConvexHull;
    s.hull = w.register_hull({v, counts, faces});
    REQUIRE(s.hull.is_valid());
    return s;
}

struct Scene {
    physics::BodyId recycled;
    physics::BodyId moving;
};

Scene fill(physics::PhysicsWorld& w) {
    w.set_gravity({0.0f, 0.0f, 0.0f});
    w.set_sleeping_enabled(false);
    const auto cube = box({0.25f, 0.25f, 0.25f});
    // The tree fattens each end by 0.1 m. A 0.05 m gap reports a candidate that the tight
    // filter can discard; the overlapping neighbours exercise unchanged manifold emission.
    for (int i = 0; i < 12; ++i)
        REQUIRE(add(w, cube, {i * 0.55f, 0.0f, 0.0f}).is_valid());
    REQUIRE(add(w, cube, {0.0f, 0.1f, 0.0f}, physics::MotionType::Static).is_valid());
    physics::ShapeDesc sphere;
    sphere.type = physics::ShapeType::Sphere;
    sphere.radius = 0.3f;
    REQUIRE(add(w, sphere, {1.1f, 0.0f, 0.0f}).is_valid());
    physics::ShapeDesc capsule;
    capsule.type = physics::ShapeType::Capsule;
    capsule.radius = 0.2f;
    capsule.half_height = 0.3f;
    REQUIRE(add(w, capsule, {1.65f, 0.0f, 0.0f}).is_valid());
    REQUIRE(add(w, hull(w), {2.2f, 0.0f, 0.0f}).is_valid());
    // Beyond the reliable-float guard, the new filter intentionally fails open.
    REQUIRE(add(w, cube, {10000.0f, 0.0f, 0.0f}).is_valid());
    REQUIRE(add(w, cube, {10000.55f, 0.0f, 0.0f}).is_valid());
    Scene scene;
    scene.recycled = add(w, cube, {3.3f, 0.0f, 0.0f});
    scene.moving = add(w, cube, {3.85f, 0.0f, 0.0f});
    REQUIRE(scene.recycled.is_valid());
    REQUIRE(scene.moving.is_valid());
    return scene;
}

void same_manifolds(const std::vector<physics::Manifold>& a,
                    const std::vector<physics::Manifold>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].a == b[i].a);
        CHECK(a[i].b == b[i].b);
        CHECK(a[i].normal.x == b[i].normal.x);
        CHECK(a[i].normal.y == b[i].normal.y);
        CHECK(a[i].normal.z == b[i].normal.z);
        CHECK(a[i].count == b[i].count);
        CHECK(a[i].child_a == b[i].child_a);
        CHECK(a[i].child_b == b[i].child_b);
        CHECK(a[i].patch == b[i].patch);
        for (std::uint8_t j = 0; j < a[i].count; ++j) {
            const auto& x = a[i].points[j];
            const auto& y = b[i].points[j];
            CHECK(x.position.x == y.position.x);
            CHECK(x.position.y == y.position.y);
            CHECK(x.position.z == y.position.z);
            CHECK(x.penetration == y.penetration);
            CHECK(x.feature_id == y.feature_id);
            CHECK(x.normal_impulse == y.normal_impulse);
            CHECK(x.tangent_impulse == y.tangent_impulse);
        }
    }
}

void same_events(const physics::PhysicsWorld& a, const physics::PhysicsWorld& b) {
    const auto x = a.contact_events();
    const auto y = b.contact_events();
    REQUIRE(x.size() == y.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        CHECK(x[i].a == y[i].a);
        CHECK(x[i].b == y[i].b);
        CHECK(x[i].phase == y[i].phase);
        CHECK(x[i].point.x == y[i].point.x);
        CHECK(x[i].point.y == y[i].point.y);
        CHECK(x[i].point.z == y[i].point.z);
        CHECK(x[i].normal_impulse == y[i].normal_impulse);
    }
}

void compare(physics::PhysicsWorld& on, physics::PhysicsWorld& off) {
    CHECK(on.world_hash() == off.world_hash());
    std::vector<physics::Manifold> x, y;
    on.compute_contacts(x);
    off.compute_contacts(y);
    same_manifolds(x, y);
    CHECK(on.contact_profile().plain_tight_tests > 0);
    CHECK(on.contact_profile().plain_tight_rejections > 0);
    CHECK(on.contact_profile().exact_narrowphase_calls <
          off.contact_profile().exact_narrowphase_calls);
}

} // namespace

TEST_CASE(
    "plain tight cull keeps hashes, manifolds, and events across mixed shapes and slot churn") {
    physics::PhysicsWorld on, off;
    const Scene a = fill(on);
    const Scene b = fill(off);
    CHECK_FALSE(on.plain_contact_cull_enabled());
    on.set_plain_contact_cull_enabled(true);
    on.set_contact_profile_enabled(true);
    off.set_contact_profile_enabled(true);
    compare(on, off);

    for (int i = 0; i < 6; ++i) {
        on.step(1.0f / 60.0f);
        off.step(1.0f / 60.0f);
        same_events(on, off);
        compare(on, off);
    }
    on.destroy_body(a.recycled);
    off.destroy_body(b.recycled);
    REQUIRE(add(on, box({0.25f, 0.25f, 0.25f}), {3.3f, 0.0f, 0.0f}, physics::MotionType::Static)
                .is_valid());
    REQUIRE(add(off, box({0.25f, 0.25f, 0.25f}), {3.3f, 0.0f, 0.0f}, physics::MotionType::Static)
                .is_valid());
    physics::BodyState sa{}, sb{};
    REQUIRE(on.get_body_state(a.moving, sa));
    REQUIRE(off.get_body_state(b.moving, sb));
    sa.position = {3.35f, 0.0f, 0.0f};
    sb.position = sa.position;
    REQUIRE(on.set_body_state(a.moving, sa));
    REQUIRE(off.set_body_state(b.moving, sb));
    compare(on, off); // fresh tight bounds after a recycled slot and a teleport
}

TEST_CASE("plain tight cull bypasses moving CCD candidates") {
    physics::PhysicsWorld on, off;
    for (auto* w : {&on, &off}) {
        w->set_gravity({0.0f, 0.0f, 0.0f});
        REQUIRE(add(*w, box({2.0f, 2.0f, 0.05f}), {}, physics::MotionType::Static).is_valid());
        physics::ShapeDesc sphere;
        sphere.type = physics::ShapeType::Sphere;
        sphere.radius = 0.1f;
        REQUIRE(add(*w,
                    sphere,
                    {0.0f, 0.0f, -1.0f},
                    physics::MotionType::Dynamic,
                    true,
                    {0.0f, 0.0f, 100.0f})
                    .is_valid());
    }
    on.set_plain_contact_cull_enabled(true);
    on.set_contact_profile_enabled(true);
    off.set_contact_profile_enabled(true);
    on.step(1.0f / 60.0f);
    off.step(1.0f / 60.0f);
    CHECK(on.world_hash() == off.world_hash());
    CHECK(on.contact_profile().speculative_calls > 0);
    CHECK(on.contact_profile().plain_tight_tests == 0);
    same_events(on, off);
}
