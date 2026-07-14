// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/core/math/vec.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp" // umbrella: body/shape/world/components/sync

// M7.6 proofs for the PhysicsSync BRIDGE — the seam that keeps an ecs::World and a PhysicsWorld in
// step across a fixed tick (bind → step → write-back → unbind), and the place where M7.5 sleeping
// and ADR-0018 §4 change detection pay off together: a settled world writes back nothing and stamps
// nothing, so a change-tracking consumer does zero work for it. All pure CPU, driving the public
// PhysicsWorld + ecs::World seams (the resting/sleeping scenes mirror the known-good ones in
// islands_test.cpp so sleep timing is not re-litigated here).
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

// A test-local marker so an entity can be pushed into its OWN archetype (hence its own chunk). The
// change-detection grain is the chunk column, so "for_each_changed visits exactly entity X" is only
// literally true when X does not share a chunk with anything else — this tag buys that separation.
struct Marker {
    std::uint32_t v = 0;
};

// Spawn an entity carrying the physics intent (RigidBody + Collider box + WorldTransform), the
// shape PhysicsSync binds a body from. Not yet bound — reconcile() does that.
ecs::Entity spawn_box(ecs::World& w, core::Vec3 pos, physics::MotionType motion, core::Vec3 half) {
    physics::RigidBody rb;
    rb.motion = static_cast<std::uint32_t>(motion);
    physics::Collider col;
    col.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Box);
    col.half_x = half.x;
    col.half_y = half.y;
    col.half_z = half.z;
    ecs::WorldTransform wt;
    wt.value.translation = pos;
    return w.spawn_with(rb, col, wt);
}

// Same, plus a Marker so this entity lands in a distinct archetype/chunk from the plain boxes.
ecs::Entity
spawn_box_tagged(ecs::World& w, core::Vec3 pos, physics::MotionType motion, core::Vec3 half) {
    const ecs::Entity e = spawn_box(w, pos, motion, half);
    w.add_component<Marker>(e, Marker{});
    return e;
}

// The body a bound entity points at (reconcile must have added the handle).
physics::BodyId body_of(ecs::World& w, ecs::Entity e) {
    return w.get<physics::RigidBodyHandle>(e)->body;
}

} // namespace

TEST_CASE("M7.6 sync: reconcile binds intent entities to bodies placed at their transform") {
    ecs::World w;
    physics::PhysicsWorld phys;
    physics::PhysicsSync sync;

    const ecs::Entity e =
        spawn_box(w, {2.0f, 5.0f, -1.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
    CHECK_FALSE(w.has<physics::RigidBodyHandle>(e));

    sync.reconcile(w, phys);

    CHECK(sync.bound_count() == 1);
    CHECK(phys.body_count() == 1);
    REQUIRE(w.has<physics::RigidBodyHandle>(e));

    physics::BodyState s;
    REQUIRE(phys.get_body_state(body_of(w, e), s));
    CHECK(s.position.x == doctest::Approx(2.0f)); // created AT the entity's WorldTransform
    CHECK(s.position.y == doctest::Approx(5.0f));
    CHECK(s.position.z == doctest::Approx(-1.0f));

    // Idempotent: an entity that already carries a handle is not re-bound.
    sync.reconcile(w, phys);
    CHECK(sync.bound_count() == 1);
    CHECK(phys.body_count() == 1);
}

TEST_CASE("M7.6 sync: write-back moves an awake body's transform and stamps it changed") {
    ecs::World w;
    physics::PhysicsWorld phys;
    physics::PhysicsSync sync;
    phys.set_gravity({0.0f, -10.0f, 0.0f});

    const ecs::Entity e =
        spawn_box(w, {0.0f, 5.0f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});

    // The canonical tick: advance the version, THEN do the tick's writes (reconcile + step +
    // write-back), so the stamps land strictly after the checkpoint a consumer took last tick.
    const ecs::Version checkpoint = w.version();
    w.advance_version();
    sync.step(w, phys, kDt);

    // The freshly-bound body is awake and fell under gravity: its WorldTransform was written back
    // and stamped, so a change-tracking consumer sees exactly this entity.
    std::vector<ecs::Entity> changed;
    float y = 0.0f;
    w.query<ecs::WorldTransform>().for_each_changed(checkpoint,
                                                    [&](ecs::Entity ent, ecs::WorldTransform& wt) {
                                                        changed.push_back(ent);
                                                        y = wt.value.translation.y;
                                                    });
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == e);
    CHECK(y < 5.0f); // moved down

    // The write-back and the body agree.
    physics::BodyState s;
    REQUIRE(phys.get_body_state(body_of(w, e), s));
    CHECK(w.get<ecs::WorldTransform>(e)->value.translation.y == doctest::Approx(s.position.y));
}

TEST_CASE("M7.6 sync: a static body is never written back, so it never stamps") {
    ecs::World w;
    physics::PhysicsWorld phys;
    physics::PhysicsSync sync;
    phys.set_gravity({0.0f, -10.0f, 0.0f});

    spawn_box(w, {0.0f, 0.0f, 0.0f}, physics::MotionType::Static, {0.5f, 0.5f, 0.5f});
    sync.reconcile(w, phys);
    CHECK(sync.bound_count() == 1);

    const ecs::Version checkpoint = w.version();
    w.advance_version();
    sync.step(w, phys, kDt);

    int seen = 0;
    w.query<ecs::WorldTransform>().for_each_changed(checkpoint,
                                                    [&](ecs::WorldTransform&) { ++seen; });
    CHECK(seen == 0); // static bodies do not move under the sim: no write-back, no stamp
}

TEST_CASE("M7.6 sync: unbind destroys the body when the entity dies or drops its intent") {
    ecs::World w;
    physics::PhysicsWorld phys;
    physics::PhysicsSync sync;

    const ecs::Entity kept =
        spawn_box(w, {0.0f, 1.0f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
    const ecs::Entity dropped =
        spawn_box(w, {2.0f, 1.0f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
    const ecs::Entity despawned =
        spawn_box(w, {4.0f, 1.0f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
    sync.reconcile(w, phys);
    CHECK(phys.body_count() == 3);
    CHECK(sync.bound_count() == 3);

    // One entity drops its RigidBody (no longer wants simulating); another is despawned entirely
    // (which takes its RigidBodyHandle with it — only the roster can find the orphaned body).
    w.remove_component<physics::RigidBody>(dropped);
    w.despawn(despawned);

    sync.reconcile(w, phys);

    CHECK(phys.body_count() == 1); // only `kept` survives
    CHECK(sync.bound_count() == 1);
    CHECK(phys.is_alive(body_of(w, kept)));
    CHECK_FALSE(w.has<physics::RigidBodyHandle>(dropped)); // the stale link was cleaned up
}

TEST_CASE("M7.6 sync: a settled world stops stamping; waking a sleeper resumes it (the payoff)") {
    // A big static floor (own chunk, via Marker) plus one dynamic box in its own chunk. This is the
    // ADR-0018 §4 + M7.5 headline: once the box sleeps, change detection goes quiet, and only a
    // wake brings exactly its chunk back — the "process only what moved" contract the editor sync
    // (M9), GPU upload, and replication (M11) all ride.
    ecs::World w;
    physics::PhysicsWorld phys;
    physics::PhysicsSync sync;
    phys.set_gravity({0.0f, -10.0f, 0.0f});

    spawn_box_tagged(w, {0.0f, -0.5f, 0.0f}, physics::MotionType::Static, {50.0f, 0.5f, 50.0f});
    const ecs::Entity box =
        spawn_box(w, {0.0f, 0.5f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
    sync.reconcile(w, phys);
    const physics::BodyId box_body = body_of(w, box);

    // Drive the fixed tick until the box settles and sleeps (bounded like islands_test's proof).
    int ticks = 0;
    for (; ticks < 300 && !phys.is_asleep(box_body); ++ticks) {
        w.advance_version();
        sync.step(w, phys, kDt);
    }
    REQUIRE(phys.is_asleep(box_body));

    // Asleep: a further tick writes nothing back, so NOTHING is stamped — a change-tracking
    // consumer does zero work for the settled world.
    ecs::Version checkpoint = w.version();
    w.advance_version();
    sync.step(w, phys, kDt);
    int seen = 0;
    w.query<ecs::WorldTransform>().for_each_changed(checkpoint,
                                                    [&](ecs::WorldTransform&) { ++seen; });
    CHECK(seen == 0);

    // Wake the box: the next tick writes it back and stamps its chunk — and ONLY its chunk (the
    // floor sits in a different archetype, so a consumer sees exactly the box, not the untouched
    // floor).
    checkpoint = w.version();
    w.advance_version();
    phys.wake_body(box_body);
    sync.step(w, phys, kDt);

    std::vector<ecs::Entity> changed;
    w.query<ecs::WorldTransform>().for_each_changed(
        checkpoint, [&](ecs::Entity ent, ecs::WorldTransform&) { changed.push_back(ent); });
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == box);
}

TEST_CASE(
    "M7.6 sync: driving the bridge is deterministic (bit-identical world hash + transforms)") {
    // The bridge must not leak nondeterminism into the sim: the same scene driven the same way
    // twice hashes identically AND lands its entity transforms bit-for-bit — the ADR-0026 contract,
    // now through the ECS seam rather than raw create_body calls.
    auto run = [](std::uint64_t& hash_out, core::Vec3& box_pos_out) {
        ecs::World w;
        physics::PhysicsWorld phys;
        physics::PhysicsSync sync;
        phys.set_gravity({0.0f, -10.0f, 0.0f});

        spawn_box(w, {0.0f, -0.5f, 0.0f}, physics::MotionType::Static, {50.0f, 0.5f, 50.0f});
        const ecs::Entity a =
            spawn_box(w, {-1.0f, 1.0f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});
        const ecs::Entity b =
            spawn_box(w, {1.0f, 2.5f, 0.0f}, physics::MotionType::Dynamic, {0.5f, 0.5f, 0.5f});

        for (int i = 0; i < 120; ++i) {
            w.advance_version();
            sync.step(w, phys, kDt);
        }
        hash_out = phys.world_hash();
        box_pos_out = w.get<ecs::WorldTransform>(a)->value.translation;
        (void)b;
    };

    std::uint64_t h1 = 0, h2 = 0;
    core::Vec3 p1{}, p2{};
    run(h1, p1);
    run(h2, p2);

    CHECK(h1 == h2);
    CHECK(p1.x == p2.x);
    CHECK(p1.y == p2.y);
    CHECK(p1.z == p2.z);
}
