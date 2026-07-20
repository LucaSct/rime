// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for m9.7 play-in-editor (ADR-0031 §4): the Edit -> Playing/Paused -> Edit state machine,
// snapshot/restore fidelity, and the physics side-table's "reconstructible from components" rule.
// PlayHarness below mirrors editor_host_main.cpp's serve_viewport orchestration EXACTLY (a
// PlaySession driving Application::step's frame_dt selection, a physics::PhysicsWorld/PhysicsSync
// recreated fresh on the Edit -> Playing/Paused transition and torn down on Stop) at a level a
// ctest can drive directly — no socket, no GPU. The wire-level Play/Pause/Step/Stop round-trip
// (byte layout, cross-language agreement) is proven separately: tests/editorhost's PlayState
// round-trip unit + tools/rime-protocol's conformance suite.

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/physics/physics.hpp"

using namespace rime;

namespace {

// A small physics-ball scene: a static box floor and a dynamic sphere suspended above it — the
// "physics-ball scene" the m9.7 plan's proof shape names. Registers exactly what
// editor_host_main.cpp's build_viewport_scene does for physics: transform components, physics
// components, and BOTH LocalTransform and WorldTransform (the flat-root pattern every viewport
// entity uses). Giving a physics-bound entity a LocalTransform too is what makes its placement
// restorable at all: WorldTransform is derived state and deliberately UNREFLECTED (reflect.hpp), so
// it cannot appear in a reflection-driven snapshot; PhysicsSync::write_back (m9.7) mirrors the pose
// into LocalTransform as well (see its comment in engine/physics/src/sync.cpp) precisely so a
// physics body's placement has a reflected, persistable source of truth for Play/Stop to restore —
// and so ecs::propagate_transforms' every-tick WorldTransform = LocalTransform reset (which runs
// BEFORE the physics tick, Application::run_ticks) is always a no-op instead of a corruption once a
// settled/sleeping body stops being write-back'd (a real bug this proof caught early — a stale
// LocalTransform used to snap a settled body back to its spawn pose the very next tick).
void build_ball_scene(ecs::World& world) {
    ecs::register_transform_components(world);
    (void)world.register_component<ecs::WorldTransform>();
    physics::register_physics_components(world);

    physics::RigidBody floor_rb;
    floor_rb.motion = static_cast<std::uint32_t>(physics::MotionType::Static);
    physics::Collider floor_col;
    floor_col.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Box);
    floor_col.half_x = 5.0f;
    floor_col.half_y = 0.1f;
    floor_col.half_z = 5.0f;
    ecs::WorldTransform floor_wt;
    floor_wt.value.translation = {0.0f, -1.0f, 0.0f};
    (void)world.spawn_with(floor_rb, floor_col, floor_wt, ecs::LocalTransform{floor_wt.value});

    physics::RigidBody ball_rb;
    ball_rb.motion = static_cast<std::uint32_t>(physics::MotionType::Dynamic);
    ball_rb.mass = 1.0f;
    physics::Collider ball_col;
    ball_col.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Sphere);
    ball_col.radius = 0.5f;
    ecs::WorldTransform ball_wt;
    ball_wt.value.translation = {0.0f, 5.0f, 0.0f}; // well above the floor — falls under gravity
    (void)world.spawn_with(ball_rb, ball_col, ball_wt, ecs::LocalTransform{ball_wt.value});
}

// Mirrors editor_host_main.cpp's derive_world_transforms exactly: a restore (PlaySession::stop, or
// a .rscene load) only reconstructs REFLECTED components, and WorldTransform is deliberately
// unreflected (derived state — reflect.hpp), so it never rides a snapshot. Give every
// LocalTransform holder that lacks one a default WorldTransform, then propagate composes the local
// chain into it. PlayHarness::stop() below calls this, exactly as the real orchestrator's Stop
// handler does — without it, a restored physics entity has no WorldTransform at all (a gap this
// proof caught: see the m9.7 report for the two-part trace, propagate-clobber then this one).
void derive_world_transforms(ecs::World& world) {
    std::vector<ecs::Entity> posed;
    world.query<ecs::LocalTransform>().for_each([&](ecs::Entity e, ecs::LocalTransform&) {
        if (!world.has<ecs::WorldTransform>(e)) {
            posed.push_back(e);
        }
    });
    for (const ecs::Entity e : posed) {
        (void)world.add_component<ecs::WorldTransform>(e, ecs::WorldTransform{});
    }
    core::JobSystem jobs;
    ecs::propagate_transforms(world, jobs);
}

// Find the (assumed unique) dynamic RigidBody's WorldTransform. Returns {0,-1,0} / false if none —
// callers only use the y value after checking `found`.
struct BallPose {
    float y = 0.0f;
    bool found = false;
};

BallPose find_ball(ecs::World& world) {
    BallPose out;
    world.query<physics::RigidBody, ecs::WorldTransform>().for_each(
        [&](physics::RigidBody& rb, ecs::WorldTransform& wt) {
            if (rb.motion == static_cast<std::uint32_t>(physics::MotionType::Dynamic)) {
                out.y = wt.value.translation.y;
                out.found = true;
            }
        });
    return out;
}

// Mirrors editor_host_main.cpp's serve_viewport orchestration: one Application, one PlaySession, a
// physics world created fresh on the Edit -> Playing/Paused transition and torn down on Stop, and
// the SAME frame_dt-selection rule (Playing or an armed step -> fixed_dt, else 0). Bundled so each
// TEST_CASE reads as the message sequence it exercises, not the plumbing.
struct PlayHarness {
    app::Application app{
        app::AppConfig{}}; // headless, GPU-free — the play state machine needs none
    editorhost::PlaySession session;
    std::unique_ptr<physics::PhysicsWorld> physics_world;
    physics::PhysicsSync physics_sync;

    PlayHarness() {
        app.on_fixed_tick([this](ecs::World& world, double dt) {
            if (physics_world) {
                physics_sync.step(world, *physics_world, static_cast<float>(dt));
            }
            session.record_tick();
        });
    }

    // The was-Edit-before-this-message check + fresh physics_world/sync, shared by the Play and
    // Step handlers below — exactly editor_host_main.cpp's `was_edit` branches.
    void begin_play_if_edit() {
        if (session.phase() == editorhost::PlayPhase::Edit) {
            physics_world = std::make_unique<physics::PhysicsWorld>();
            physics_world->set_gravity({0.0f, -10.0f, 0.0f});
            physics_sync = physics::PhysicsSync{};
        }
    }

    // The Play message's effect.
    void play() {
        begin_play_if_edit();
        session.play(app.world());
        app.timestep().reset();
    }

    // The Pause message's effect.
    void pause() {
        begin_play_if_edit();
        session.pause(app.world());
        app.timestep().reset();
    }

    // The Step message's effect: ensure a paused, playable session, then run exactly one tick right
    // now (the accumulator was just reset to 0, so passing fixed_dt produces exactly 1 tick).
    void step_once() {
        pause();
        app.step(app.fixed_dt());
    }

    // Play, then run `n` continuous ticks (frame_dt == fixed_dt every call, like the viewport
    // loop's Playing branch).
    void run_ticks(int n) {
        play();
        for (int i = 0; i < n; ++i) {
            app.step(app.fixed_dt());
        }
    }

    // The Stop message's effect.
    bool stop() {
        const bool did = session.stop(app.world());
        if (did) {
            derive_world_transforms(app.world()); // restore only reconstructs reflected components
            physics_world.reset();
            physics_sync = physics::PhysicsSync{};
        }
        app.timestep().reset();
        return did;
    }
};

} // namespace

TEST_CASE("m9.7 play: 100 ticks then stop restores the pre-play world's content exactly") {
    PlayHarness h;
    build_ball_scene(h.app.world());
    const std::uint64_t pre_hash = editorhost::world_content_hash(h.app.world());

    h.run_ticks(100);

    // The ball actually fell — a no-op sim would pass the restore check vacuously.
    const BallPose mid = find_ball(h.app.world());
    REQUIRE(mid.found);
    CHECK(mid.y < 5.0f);
    CHECK(h.session.tick_count() == 100);

    REQUIRE(h.stop());
    CHECK(h.session.phase() == editorhost::PlayPhase::Edit);
    CHECK(h.session.tick_count() == 0); // the counter resets with the session

    const std::uint64_t post_hash = editorhost::world_content_hash(h.app.world());
    CHECK(pre_hash == post_hash); // the ADR-0031 §4 restore proof

    // The physics side-table rebuilt from components (this brick's real engineering): the restored
    // ball is back at y=5 before any new tick runs...
    const BallPose restored = find_ball(h.app.world());
    REQUIRE(restored.found);
    CHECK(restored.y == doctest::Approx(5.0f));

    // ...and a fresh PhysicsWorld/PhysicsSync, given nothing but the restored components, binds
    // both bodies and simulates correctly from here: one more tick and the ball falls again.
    h.step_once();
    CHECK(h.physics_sync.bound_count() == 2); // floor + ball, rebound from RigidBody/Collider alone
    const BallPose after_restore_tick = find_ball(h.app.world());
    REQUIRE(after_restore_tick.found);
    CHECK(after_restore_tick.y < 5.0f);
}

TEST_CASE("m9.7 play: N single-steps equal one N-tick run (ADR-0023's proof, now Step-driven)") {
    PlayHarness stepped;
    build_ball_scene(stepped.app.world());
    for (int i = 0; i < 30; ++i) {
        stepped.step_once();
    }
    CHECK(stepped.session.tick_count() == 30);

    PlayHarness continuous;
    build_ball_scene(continuous.app.world());
    continuous.run_ticks(30);
    CHECK(continuous.session.tick_count() == 30);

    // Same worlds built the same way -> same archetype/iteration order, so column i corresponds.
    // The claim is BIT-identical, not approximately equal (application_test.cpp's tick-determinism
    // precedent): identical ops, identical count, identical order.
    std::vector<core::Vec3> a, b;
    stepped.app.world().query<ecs::WorldTransform>().for_each(
        [&](ecs::WorldTransform& wt) { a.push_back(wt.value.translation); });
    continuous.app.world().query<ecs::WorldTransform>().for_each(
        [&](ecs::WorldTransform& wt) { b.push_back(wt.value.translation); });
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == 2u);
    bool all_bit_identical = true;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) {
            all_bit_identical = false;
        }
    }
    CHECK(all_bit_identical);
    // And the ball actually moved — a frozen world would pass vacuously.
    const BallPose bp = find_ball(stepped.app.world());
    REQUIRE(bp.found);
    CHECK(bp.y < 5.0f);
}

TEST_CASE("m9.7 play: pause holds state — two snapshots across a pause are identical") {
    PlayHarness h;
    build_ball_scene(h.app.world());
    h.run_ticks(10);
    h.pause();
    const std::vector<std::byte> s1 = editorhost::serialize_world(h.app.world());
    // Time passes with no ticks (Paused): render-only iterations, exactly what the viewport loop
    // does between messages (frame_dt == 0.0).
    for (int i = 0; i < 5; ++i) {
        h.app.step(0.0);
    }
    const std::vector<std::byte> s2 = editorhost::serialize_world(h.app.world());
    CHECK(s1 == s2); // no entity churn happened here, so raw bytes (incl. handles) may compare
    CHECK(h.session.phase() == editorhost::PlayPhase::Paused);
    CHECK(h.session.tick_count() == 10); // unchanged by the paused iterations
}

TEST_CASE("m9.7 play: edits made during Play are discarded on Stop") {
    PlayHarness h;
    build_ball_scene(h.app.world());
    const std::size_t pre_count = h.app.world().entity_count();
    h.play();
    (void)h.app.world().spawn(); // an edit applied mid-play (SetComponent/Spawn land the same way)
    CHECK(h.app.world().entity_count() == pre_count + 1);

    REQUIRE(h.stop());
    CHECK(h.app.world().entity_count() == pre_count); // the spawn did not survive Stop
}

TEST_CASE("m9.7 play: the fixed-tick accumulator never leaks across state transitions") {
    PlayHarness h;
    build_ball_scene(h.app.world());

    h.play();
    // Playing: editor_host_main.cpp always passes EXACTLY fixed_dt or 0.0 per iteration (never a
    // raw wall-clock dt), so the accumulator should read back to exactly 0 between calls with no
    // drift at all (the same double value is added then subtracted — bit-exact, not just close).
    for (int i = 0; i < 7; ++i) {
        h.app.step(h.app.fixed_dt());
    }
    CHECK(h.app.timestep().accumulator == 0.0);

    h.pause();
    CHECK(h.app.timestep().accumulator == 0.0); // explicit reset, not just luck
    for (int i = 0; i < 3; ++i) {
        h.app.step(0.0); // paused: render-only, no ticks, no accumulation
    }
    CHECK(h.app.timestep().accumulator == 0.0);

    // Step: exactly one tick, not zero, not two.
    const std::uint64_t before = h.session.tick_count();
    h.step_once();
    CHECK(h.session.tick_count() == before + 1);
    CHECK(h.app.timestep().accumulator == 0.0);

    REQUIRE(h.stop());
    CHECK(h.app.timestep().accumulator == 0.0);
}
