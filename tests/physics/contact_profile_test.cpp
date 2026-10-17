// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/physics/physics.hpp"

using namespace rime;

namespace {

physics::ShapeDesc box(core::Vec3 half) {
    physics::ShapeDesc shape;
    shape.type = physics::ShapeType::Box;
    shape.half_extents = half;
    return shape;
}

physics::BodyId add(physics::PhysicsWorld& world,
                    physics::ShapeDesc shape,
                    core::Vec3 position,
                    physics::MotionType motion = physics::MotionType::Dynamic) {
    physics::BodyDesc desc;
    desc.shape = shape;
    desc.position = position;
    desc.motion = motion;
    return world.create_body(desc);
}

void fill_scene(physics::PhysicsWorld& world) {
    world.set_sleeping_enabled(false);
    const auto unit = box({0.3f, 0.3f, 0.3f});
    // Two distant children give the union proxy a broad overlap while most child Cartesian
    // products cannot touch. The exact cull count proves that the diagnostic sees this stage.
    const std::vector<physics::CompoundChildDesc> children = {
        {unit, {-2.0f, 0.0f, 0.0f}, core::quat_identity()},
        {unit, {2.0f, 0.0f, 0.0f}, core::quat_identity()},
    };
    const auto compound = world.register_compound(physics::CompoundDesc{children});
    REQUIRE(compound.is_valid());
    physics::ShapeDesc compound_shape;
    compound_shape.type = physics::ShapeType::Compound;
    compound_shape.compound = compound;
    REQUIRE(add(world, compound_shape, {0.0f, 0.35f, 0.0f}).is_valid());
    REQUIRE(add(world, compound_shape, {0.1f, 0.35f, 0.0f}).is_valid());
    REQUIRE(add(world, box({3.0f, 0.2f, 1.0f}), {0.0f, -0.15f, 0.0f}, physics::MotionType::Static)
                .is_valid());
    REQUIRE(add(world, unit, {0.0f, 0.45f, 0.0f}).is_valid());
    REQUIRE(add(world, unit, {0.2f, 0.45f, 0.0f}).is_valid());

    const std::vector<std::uint16_t> samples(9, 0);
    physics::HeightfieldDesc terrain;
    terrain.samples = samples;
    terrain.columns = 3;
    terrain.rows = 3;
    const auto terrain_id = world.register_heightfield(terrain);
    REQUIRE(terrain_id.is_valid());
    physics::ShapeDesc terrain_shape;
    terrain_shape.type = physics::ShapeType::Heightfield;
    terrain_shape.heightfield = terrain_id;
    REQUIRE(add(world, terrain_shape, {5.0f, 0.0f, 0.0f}, physics::MotionType::Static).is_valid());
    REQUIRE(add(world, unit, {5.5f, 0.25f, 0.5f}).is_valid());
}

void check_manifolds(const std::vector<physics::Manifold>& a,
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

} // namespace

TEST_CASE("contact profile counts all dispatch arms without changing contact results") {
    physics::PhysicsWorld on;
    physics::PhysicsWorld off;
    fill_scene(on);
    fill_scene(off);
    REQUIRE_FALSE(on.contact_profile_enabled());
    CHECK(on.contact_profile().unique_pairs == 0);
    on.set_contact_profile_enabled(true);

    for (int step = 0; step < 4; ++step) {
        on.step(1.0f / 60.0f);
        off.step(1.0f / 60.0f);
        CHECK(on.world_hash() == off.world_hash());
        const auto& profile = on.contact_profile();
        CHECK(profile.raw_candidate_keys == profile.unique_pairs + profile.duplicate_keys);
        CHECK(profile.unique_pairs ==
              profile.plain_pairs + profile.compound_pairs + profile.heightfield_pairs);
        CHECK(profile.plain_pairs > 0);
        CHECK(profile.compound_pairs > 0);
        CHECK(profile.heightfield_pairs > 0);
        CHECK(profile.duplicate_keys > 0);
        CHECK(profile.posed_children > 0);
        CHECK(profile.tested_child_pairs > profile.culled_child_pairs);
        CHECK(profile.culled_child_pairs > 0);
        CHECK(profile.exact_narrowphase_calls > 0);
        CHECK(profile.produced_manifolds > 0);
        CHECK(profile.pair_query_ns > 0);
        CHECK(profile.pair_canonicalize_ns > 0);
        CHECK(profile.compound_pose_ns > 0);
        CHECK(profile.compound_midphase_ns > 0);
        CHECK(profile.plain_exact_ns > 0);
        CHECK(profile.compound_exact_ns > 0);
        CHECK(profile.heightfield_ns > 0);

        const auto events_a = on.contact_events();
        const auto events_b = off.contact_events();
        REQUIRE(events_a.size() == events_b.size());
        for (std::size_t i = 0; i < events_a.size(); ++i) {
            CHECK(events_a[i].a == events_b[i].a);
            CHECK(events_a[i].b == events_b[i].b);
            CHECK(events_a[i].phase == events_b[i].phase);
            CHECK(events_a[i].child_a == events_b[i].child_a);
            CHECK(events_a[i].child_b == events_b[i].child_b);
            CHECK(events_a[i].patch == events_b[i].patch);
            CHECK(events_a[i].point.x == events_b[i].point.x);
            CHECK(events_a[i].point.y == events_b[i].point.y);
            CHECK(events_a[i].point.z == events_b[i].point.z);
            CHECK(events_a[i].normal.x == events_b[i].normal.x);
            CHECK(events_a[i].normal.y == events_b[i].normal.y);
            CHECK(events_a[i].normal.z == events_b[i].normal.z);
            CHECK(events_a[i].normal_impulse == events_b[i].normal_impulse);
            CHECK(events_a[i].tangent_impulse == events_b[i].tangent_impulse);
        }

        std::vector<physics::Manifold> manifolds_a;
        std::vector<physics::Manifold> manifolds_b;
        on.compute_contacts(manifolds_a);
        off.compute_contacts(manifolds_b);
        check_manifolds(manifolds_a, manifolds_b);
        CHECK(on.contact_profile().produced_manifolds == manifolds_a.size());
        CHECK(on.contact_profile().speculative_calls == 0); // dt=0 inspection seam
        const auto pair_count = on.contact_profile().unique_pairs;
        const auto query_time = on.contact_profile().pair_query_ns;
        std::vector<physics::PhysicsWorld::Pair> pairs;
        on.compute_pairs(pairs); // a standalone broadphase query must not overwrite the build
        CHECK(on.contact_profile().unique_pairs == pair_count);
        CHECK(on.contact_profile().pair_query_ns == query_time);
        CHECK(off.contact_profile().raw_candidate_keys == 0);
        CHECK(off.contact_profile().pair_query_ns == 0);
    }

    on.set_contact_profile_enabled(false);
    CHECK_FALSE(on.contact_profile_enabled());
    CHECK(on.contact_profile().raw_candidate_keys == 0);
    CHECK(on.contact_profile().heightfield_ns == 0);
    std::vector<physics::Manifold> disabled_manifolds;
    on.compute_contacts(disabled_manifolds);
    CHECK(on.contact_profile().raw_candidate_keys == 0);
    CHECK(on.contact_profile().heightfield_ns == 0);
}

TEST_CASE("contact profile counts speculative dispatch on an approaching CCD pair") {
    physics::PhysicsWorld on;
    physics::PhysicsWorld off;
    for (auto* world : {&on, &off}) {
        world->set_gravity({0.0f, 0.0f, 0.0f});
        REQUIRE(
            add(*world, box({2.0f, 2.0f, 0.05f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static)
                .is_valid());
        physics::BodyDesc projectile;
        projectile.shape.type = physics::ShapeType::Sphere;
        projectile.shape.radius = 0.1f;
        projectile.position = {0.0f, 0.0f, -1.0f};
        projectile.linear_velocity = {0.0f, 0.0f, 100.0f};
        projectile.ccd = true;
        REQUIRE(world->create_body(projectile).is_valid());
    }
    on.set_contact_profile_enabled(true);
    on.step(1.0f / 60.0f);
    off.step(1.0f / 60.0f);
    CHECK(on.world_hash() == off.world_hash());
    CHECK(on.contact_profile().speculative_calls > 0);
    CHECK(on.contact_profile().plain_speculative_ns > 0);
    CHECK(on.contact_profile().produced_manifolds > 0);
    CHECK(off.contact_profile().speculative_calls == 0);
}
