// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include "../../samples/contact_bench/workload.hpp"
#include "rime/physics/physics.hpp"

namespace physics = rime::physics;
namespace core = rime::core;

namespace {
physics::ShapeDesc unit_box() {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.half_extents = {0.5f, 0.5f, 0.5f};
    return s;
}

physics::BodyId
add(physics::PhysicsWorld& w, physics::ShapeDesc s, core::Vec3 p, bool fixed = false) {
    physics::BodyDesc d;
    d.shape = s;
    d.position = p;
    if (fixed)
        d.motion = physics::MotionType::Static;
    return w.create_body(d);
}

void teleport(physics::PhysicsWorld& w, physics::BodyId id, core::Vec3 pos) {
    physics::BodyState state;
    REQUIRE(w.get_body_state(id, state));
    state.position = pos;
    REQUIRE(w.set_body_state(id, state));
}
} // namespace

TEST_CASE("plain bounds reset across inspection teleports slot reuse and dense compaction") {
    physics::PhysicsWorld w;
    w.set_gravity({});
    w.set_contact_profile_enabled(true);
    const auto fixed = add(w, unit_box(), {}, true);
    const auto moving = add(w, unit_box(), {1.05f, 0, 0});
    const auto far = add(w, unit_box(), {100, 0, 0});
    REQUIRE(fixed.is_valid());
    REQUIRE(moving.is_valid());
    REQUIRE(far.is_valid());
    std::vector<physics::Manifold> contacts;
    w.compute_contacts(contacts);
    CHECK(contacts.empty());
    CHECK(w.contact_profile().plain_bounds_rejected == 1);
    CHECK(w.contact_profile().exact_narrowphase_calls == 0);
    CHECK(w.contact_profile().plain_bounds_bodies == 2); // far body is never posed by this cache
    teleport(w, moving, {0.95f, 0, 0});
    w.compute_contacts(contacts);
    REQUIRE(contacts.size() == 1);
    CHECK(contacts[0].count > 0);
    CHECK(w.contact_profile().plain_bounds_rejected == 0);
    teleport(w, moving, {1.05f, 0, 0});
    w.compute_contacts(contacts);
    CHECK(contacts.empty());
    w.destroy_body(moving); // the far body's dense index compacts into the hole
    physics::ShapeDesc sphere;
    sphere.radius = 0.25f;
    const auto reused = add(w, sphere, {0.7f, 0, 0});
    REQUIRE(reused.is_valid());
    CHECK(reused.index == moving.index);
    CHECK(reused.generation != moving.generation);
    w.compute_contacts(contacts);
    REQUIRE(contacts.size() == 1);
    CHECK(contacts[0].b == reused);
    CHECK(w.contact_profile().plain_bounds_bodies == 2);
    teleport(w, fixed, {-10, 0, 0});
    w.compute_contacts(contacts);
    CHECK(contacts.empty());
    CHECK(w.contact_profile().plain_bounds_bodies == 0);
    teleport(w, fixed, {});
    w.compute_contacts(contacts);
    CHECK(contacts.size() == 1);
}

TEST_CASE("plain bound rejection preserves approaching speculative CCD") {
    for (bool ccd : {false, true}) {
        CAPTURE(ccd);
        physics::PhysicsWorld w;
        w.set_gravity({});
        w.set_contact_profile_enabled(true);
        auto wall = unit_box();
        wall.half_extents = {0.05f, 2, 2};
        REQUIRE(add(w, wall, {}, true).is_valid());
        physics::BodyDesc bullet;
        bullet.shape.radius = 0.1f;
        bullet.position = {-0.5f, 0, 0};
        bullet.linear_velocity = {120, 0, 0};
        bullet.ccd = ccd;
        const auto id = w.create_body(bullet);
        REQUIRE(id.is_valid());
        w.step(1.0f / 60);
        physics::BodyState state;
        REQUIRE(w.get_body_state(id, state));
        if (ccd) {
            CHECK(state.position.x < 0);
            CHECK(w.contact_profile().plain_bounds_rejected > 0);
            CHECK(w.contact_profile().speculative_calls > 0);
            CHECK(w.stats().contact_points > 0);
        } else
            CHECK(state.position.x > 0); // sensitivity: same fast body tunnels without CCD
    }
}

TEST_CASE("cached convex bounds keep reference contacts over shape rotation and gap sweeps") {
    namespace cw = contact_workload;
    cw::Workload w;
    physics::ConvexHull hull;
    std::vector<core::Vec3> vertices;
    for (unsigned i = 0; i < 8; ++i) {
        const float x = (i & 1u) ? 0.4f : -0.4f, y = (i & 2u) ? 0.4f : -0.4f,
                    z = (i & 4u) ? 0.4f : -0.4f;
        vertices.push_back({x + 0.2f * y, y, z});
    }
    const std::vector<std::uint32_t> counts = {4, 4, 4, 4, 4, 4};
    const std::vector<std::uint32_t> indices = {0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4,
                                                2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    REQUIRE(physics::build_convex_hull(vertices, counts, indices, hull));
    w.hulls.push_back(hull);
    physics::contact_bounds_detail::Cache cache;
    unsigned hits = 0, skips = 0;
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = 0; b < 4; ++b)
            for (float angle : {0.0f, 0.37f, 1.1f})
                for (float offset : {0.0f, 128.0f})
                    for (float gap : {-0.05f, -1e-5f, 0.0f, 1e-5f, 5e-4f, 2e-3f, 0.05f, 0.2f}) {
                        CAPTURE(a);
                        CAPTURE(b);
                        CAPTURE(angle);
                        CAPTURE(offset);
                        CAPTURE(gap);
                        cw::Pair p;
                        p.a = {0, 0};
                        p.b = {1, 0};
                        p.sa.desc.type = static_cast<physics::ShapeType>(a);
                        p.sb.desc.type = static_cast<physics::ShapeType>(b);
                        p.sa.desc.radius = p.sb.desc.radius = 0.25f;
                        p.sa.desc.half_extents = p.sb.desc.half_extents = {0.4f, 0.4f, 0.4f};
                        p.sa.desc.half_height = p.sb.desc.half_height = 0.35f;
                        if (a == 3)
                            p.sa.hull = 0;
                        if (b == 3)
                            p.sb.hull = 0;
                        p.qa = core::quat_from_axis_angle({0, 1, 0}, angle);
                        p.qb = core::quat_from_axis_angle({1, 0, 0}, angle * 0.7f);
                        p.pa = {offset, 0, 0};
                        const auto* ha = cw::hull(w, p.sa);
                        const auto* hb = cw::hull(w, p.sb);
                        const auto ba = ha ? physics::hull_world_aabb(*ha, p.pa, p.qa)
                                           : physics::compute_aabb(p.sa.desc, p.pa, p.qa);
                        const auto bb = hb ? physics::hull_world_aabb(*hb, {}, p.qb)
                                           : physics::compute_aabb(p.sb.desc, {}, p.qb);
                        p.pb = {ba.max.x - bb.min.x + gap, 0, 0};
                        cache.begin(2);
                        const auto& ca = cache.get(0, p.sa.desc, p.pa, p.qa, ha);
                        const auto& cb = cache.get(1, p.sb.desc, p.pb, p.qb, hb);
                        const bool skip = !physics::overlaps(ca.padded, cb.tight);
                        const auto reference = cw::evaluate(w, p, cw::Candidate::None);
                        const auto candidate = cw::evaluate_decision(w, p, skip);
                        CHECK(candidate.contact == reference.contact);
                        if (reference.contact) {
                            ++hits;
                            CHECK(cw::same_manifold(candidate.manifold, reference.manifold));
                        }
                        if (skip)
                            ++skips;
                    }
    CHECK(hits > 0);
    CHECK(skips > 0);
}
