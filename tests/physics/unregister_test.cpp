// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/physics/physics.hpp"

// M8.5 proofs: unregister_hull / unregister_compound — the shape stores become generational slot
// tables so the M8 destruction lifecycle can free debris geometry and keep the stores bounded under
// continuous refracture. What we pin: a freed id reads dead, a freed slot is REUSED (so the store
// stays flat), reuse bumps the generation (no stale-id aliasing), reject-if-referenced protects
// live bodies and compounds, and the id sequence is a pure function of the calls (determinism).
using namespace rime;

namespace {

std::vector<core::Vec3> cube_verts(core::Vec3 half) {
    std::vector<core::Vec3> v;
    for (std::uint32_t i = 0; i < 8; ++i) {
        v.push_back({(i & 1u) != 0 ? half.x : -half.x,
                     (i & 2u) != 0 ? half.y : -half.y,
                     (i & 4u) != 0 ? half.z : -half.z});
    }
    return v;
}

physics::HullId register_cube(physics::PhysicsWorld& w, core::Vec3 half = {0.5f, 0.5f, 0.5f}) {
    const std::vector<std::uint32_t> counts = {4, 4, 4, 4, 4, 4};
    const std::vector<std::uint32_t> indices = {0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4,
                                                2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    return w.register_hull(physics::HullDesc{cube_verts(half), counts, indices});
}

physics::ShapeDesc hull_shape(physics::HullId h) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::ConvexHull;
    s.hull = h;
    return s;
}

physics::CompoundId register_pair(physics::PhysicsWorld& w, physics::HullId a, physics::HullId b) {
    std::vector<physics::CompoundChildDesc> children = {
        {hull_shape(a), {-1.0f, 0.0f, 0.0f}, core::quat_identity()},
        {hull_shape(b), {1.0f, 0.0f, 0.0f}, core::quat_identity()}};
    return w.register_compound(physics::CompoundDesc{children});
}

physics::BodyId add_body(physics::PhysicsWorld& w, const physics::ShapeDesc& shape) {
    physics::BodyDesc d;
    d.shape = shape;
    d.motion = physics::MotionType::Dynamic;
    d.mass = 1.0f;
    return w.create_body(d);
}

} // namespace

TEST_CASE("M8.5 unregister_hull: a freed hull id reads dead everywhere") {
    physics::PhysicsWorld w;
    const physics::HullId h = register_cube(w);
    REQUIRE(h.is_valid());
    physics::HullInfo info;
    CHECK(w.hull_info(h, info)); // live

    CHECK(w.unregister_hull(h));                        // freed
    CHECK_FALSE(w.hull_info(h, info));                  // the id is now dead
    CHECK_FALSE(add_body(w, hull_shape(h)).is_valid()); // a body can't be built on it
    CHECK_FALSE(w.unregister_hull(h));                  // double-unregister is a safe no-op
}

TEST_CASE("M8.5 unregister_hull: refused while a live body references it") {
    physics::PhysicsWorld w;
    const physics::HullId h = register_cube(w);
    const physics::BodyId b = add_body(w, hull_shape(h));
    REQUIRE(b.is_valid());

    CHECK_FALSE(w.unregister_hull(h)); // a live body holds it — refused, and nothing changed
    physics::HullInfo info;
    CHECK(w.hull_info(h, info)); // still live

    w.destroy_body(b);
    CHECK(w.unregister_hull(h)); // the last reference gone, now it frees
}

TEST_CASE(
    "M8.5 unregister_hull: refused while a live compound references it; the compound releases "
    "it when freed") {
    physics::PhysicsWorld w;
    const physics::HullId a = register_cube(w);
    const physics::HullId b = register_cube(w);
    const physics::CompoundId c = register_pair(w, a, b);
    REQUIRE(c.is_valid());

    // Both child hulls are held by the compound.
    CHECK_FALSE(w.unregister_hull(a));
    CHECK_FALSE(w.unregister_hull(b));

    // Freeing the compound releases its hold on both — then they unregister.
    CHECK(w.unregister_compound(c));
    CHECK(w.unregister_hull(a));
    CHECK(w.unregister_hull(b));
}

TEST_CASE("M8.5 unregister_compound: refused while a live body uses it") {
    physics::PhysicsWorld w;
    const physics::HullId a = register_cube(w);
    const physics::HullId b = register_cube(w);
    const physics::CompoundId c = register_pair(w, a, b);
    physics::ShapeDesc cs;
    cs.type = physics::ShapeType::Compound;
    cs.compound = c;
    const physics::BodyId body = add_body(w, cs);
    REQUIRE(body.is_valid());

    CHECK_FALSE(w.unregister_compound(c)); // a live body uses it — refused
    w.destroy_body(body);
    CHECK(w.unregister_compound(c)); // now it frees
}

TEST_CASE("M8.5 slot reuse: a freed slot comes back with a bumped generation (bounded store)") {
    physics::PhysicsWorld w;
    const physics::HullId h0 = register_cube(w);
    CHECK(h0.index == 0);
    CHECK(h0.generation == 0);

    REQUIRE(w.unregister_hull(h0));
    const physics::HullId h1 = register_cube(w);
    CHECK(h1.index == 0);      // the SAME slot is reused — the store did not grow
    CHECK(h1.generation == 1); // …but with a fresh generation, so h0 stays dead
    CHECK(h0 != h1);
    physics::HullInfo info;
    CHECK(w.hull_info(h1, info));       // the reused id is live
    CHECK_FALSE(w.hull_info(h0, info)); // the old id is still dead

    // A body builds fine on the reused slot (the new geometry is really there). Destroy it after —
    // it holds a reference, and the cycle loop below needs the slot freeable again.
    const physics::BodyId probe = add_body(w, hull_shape(h1));
    CHECK(probe.is_valid());
    w.destroy_body(probe);

    // Many cycles keep reusing slot 0 — the store stays flat, generation climbs.
    physics::HullId h = h1;
    for (std::uint32_t gen = 2; gen < 6; ++gen) {
        REQUIRE(w.unregister_hull(h));
        h = register_cube(w);
        CHECK(h.index == 0);
        CHECK(h.generation == gen);
    }
}

TEST_CASE("M8.5 determinism: the id sequence is a pure function of the register/unregister calls") {
    const auto run = [] {
        physics::PhysicsWorld w;
        std::vector<std::uint64_t> ids;
        const auto record = [&ids](physics::HullId h) {
            ids.push_back((static_cast<std::uint64_t>(h.index) << 32) | h.generation);
        };
        const physics::HullId a = register_cube(w);
        record(a);
        const physics::HullId b = register_cube(w);
        record(b);
        (void)w.unregister_hull(a);
        record(register_cube(w)); // reuses a's slot, gen 1
        (void)w.unregister_hull(b);
        record(register_cube(w)); // reuses b's slot, gen 1
        record(register_cube(w)); // fresh slot 2
        return ids;
    };
    CHECK(run() == run()); // identical id streams across two independent worlds
}

TEST_CASE("M8.5 unregister: null, foreign, and stale ids are all safe no-ops") {
    physics::PhysicsWorld w;
    CHECK_FALSE(w.unregister_hull(physics::HullId{}));             // null
    CHECK_FALSE(w.unregister_hull(physics::HullId{99, 0}));        // out of range
    CHECK_FALSE(w.unregister_compound(physics::CompoundId{}));     // null
    CHECK_FALSE(w.unregister_compound(physics::CompoundId{7, 0})); // out of range

    const physics::HullId h = register_cube(w);
    CHECK_FALSE(w.unregister_hull(physics::HullId{h.index, 5})); // right slot, wrong generation
    physics::HullInfo info;
    CHECK(w.hull_info(h, info)); // the wrong-generation attempt left h untouched — still resolvable
    CHECK(w.unregister_hull(h));
}

TEST_CASE("M8.5 world integrity: unregister/reuse across a stepped scene stays deterministic") {
    // A scene that registers, frees, and reuses shapes while stepping must still hash identically
    // run-to-run (the free list makes ids reproducible; freed geometry must not leak into the sim).
    const auto run = [] {
        physics::PhysicsWorld w;
        const physics::HullId ground_hull = register_cube(w, {10.0f, 0.5f, 10.0f});
        physics::BodyDesc gd;
        gd.shape = hull_shape(ground_hull);
        gd.motion = physics::MotionType::Static;
        gd.position = {0.0f, 0.0f, 0.0f};
        (void)w.create_body(gd);

        const physics::HullId falling = register_cube(w);
        physics::BodyDesc fd;
        fd.shape = hull_shape(falling);
        fd.motion = physics::MotionType::Dynamic;
        fd.mass = 1.0f;
        fd.position = {0.0f, 5.0f, 0.0f};
        const physics::BodyId body = w.create_body(fd);
        for (int i = 0; i < 60; ++i) {
            w.step(1.0f / 60.0f);
        }
        // Free the faller and reuse its slot for a fresh hull — mid-life, as debris lifecycle will.
        w.destroy_body(body);
        CHECK(w.unregister_hull(falling));
        const physics::HullId reused = register_cube(w, {0.25f, 0.25f, 0.25f});
        CHECK(reused.index == falling.index); // reused the freed slot
        for (int i = 0; i < 30; ++i) {
            w.step(1.0f / 60.0f);
        }
        return w.world_hash();
    };
    CHECK(run() == run());
}
