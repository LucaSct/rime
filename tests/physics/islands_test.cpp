// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.5 proofs: simulation ISLANDS, SLEEPING, and the JOB-SYSTEM PARALLEL STEP. All pure CPU and
// structural, in the house pattern (drive the public PhysicsWorld seam; islands.hpp / the scheduler
// are exercised only through step(), exactly as the solver internals are). The headline is the
// determinism proof — stepping the same scene on 1, 2, 3, 4 worker threads and sequentially must
// produce a bit-identical world_hash(), the contract the whole parallel design exists to keep
// (ADR-0026). The rest pin the two new behaviours: independent islands, and deactivation/waking.
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

physics::ShapeDesc box(core::Vec3 half) {
    return physics::ShapeDesc{physics::ShapeType::Box, 0.0f, half, 0.0f};
}

physics::ShapeDesc sphere(float r) {
    return physics::ShapeDesc{physics::ShapeType::Sphere, r, {}, 0.0f};
}

physics::BodyId
add_dynamic(physics::PhysicsWorld& w, const physics::ShapeDesc& shape, core::Vec3 pos) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Dynamic;
    d.shape = shape;
    d.position = pos;
    return w.create_body(d);
}

physics::BodyId
add_static(physics::PhysicsWorld& w, const physics::ShapeDesc& shape, core::Vec3 pos) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape = shape;
    d.position = pos;
    return w.create_body(d);
}

// A big static floor whose top surface sits at y = 0.
physics::BodyId add_ground(physics::PhysicsWorld& w) {
    return add_static(w, box({50.0f, 0.5f, 50.0f}), {0.0f, -0.5f, 0.0f});
}

// A scene with SEVERAL independent islands so the parallel path does real, uneven work: three
// separated three-box stacks plus two free-falling spheres between them. The stacks are 6 m apart,
// far past their 1 m width, so nothing bridges them — each is its own island, and the spheres are
// two more. Bodies are created in a fixed order, so the dense layout (and thus world_hash()) is
// reproducible run to run.
void build_multi_island_scene(physics::PhysicsWorld& w) {
    add_ground(w);
    for (float cx : {-6.0f, 0.0f, 6.0f}) {
        add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {cx, 0.5f, 0.0f});
        add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {cx, 1.5f, 0.0f});
        add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {cx, 2.5f, 0.0f});
    }
    add_dynamic(w, sphere(0.5f), {-3.0f, 5.0f, 0.0f});
    add_dynamic(w, sphere(0.5f), {3.0f, 5.0f, 0.0f});
}

} // namespace

TEST_CASE("M7.5: separated dynamic bodies form separate islands, contacting ones merge") {
    // Two boxes far apart on a shared static floor: the floor is not an island node, so it does not
    // merge them — two dynamic bodies, two islands.
    {
        physics::PhysicsWorld w;
        add_ground(w);
        add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {-5.0f, 0.5f, 0.0f});
        add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {5.0f, 0.5f, 0.0f});
        w.step(kDt);
        CHECK(w.islands_last() == 2);
    }
    // Two boxes stacked (a dynamic–dynamic contact) collapse into one island.
    {
        physics::PhysicsWorld w;
        add_ground(w);
        add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.5f, 0.0f});
        add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.5f, 0.0f});
        w.step(kDt);
        CHECK(w.islands_last() == 1);
    }
}

TEST_CASE("M7.5: a resting body goes to sleep and then costs nothing") {
    physics::PhysicsWorld w; // sleeping on by default
    add_ground(w);
    const physics::BodyId b = add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.5f, 0.0f});

    int steps = 0;
    for (; steps < 300 && !w.is_asleep(b); ++steps) {
        w.step(kDt);
    }
    CHECK(w.is_asleep(b));
    CHECK(steps < 300); // it slept well within the budget (≈ settle + kTimeToSleep)

    // Once asleep it is frozen: the whole world (static floor + this one body) stops changing, so
    // its hash is invariant tick over tick — the "resting stacks cost nothing" property.
    const std::uint64_t h = w.world_hash();
    for (int i = 0; i < 60; ++i) {
        w.step(kDt);
        CHECK(w.world_hash() == h);
    }
    CHECK(w.is_asleep(b));
}

TEST_CASE("M7.5: a body dropped onto a sleeping body wakes it, then both re-sleep") {
    physics::PhysicsWorld w;
    add_ground(w);
    const physics::BodyId a = add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.5f, 0.0f});

    for (int i = 0; i < 200 && !w.is_asleep(a); ++i) {
        w.step(kDt);
    }
    REQUIRE(w.is_asleep(a));

    // Drop a second box from ~1 m up. It falls as its own (awake) island; when it lands it merges
    // into a's island, whose now-moving member reactivates a.
    const physics::BodyId bdrop = add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 2.5f, 0.0f});
    CHECK(w.is_asleep(a));      // still asleep while the faller is separate
    CHECK(!w.is_asleep(bdrop)); // a freshly created body starts awake
    for (int i = 0; i < 40; ++i) {
        w.step(kDt);
    }
    CHECK(!w.is_asleep(a)); // the impact woke it

    // Left alone, the two-box stack settles and the whole island sleeps again.
    for (int i = 0; i < 300; ++i) {
        w.step(kDt);
    }
    CHECK(w.is_asleep(a));
    CHECK(w.is_asleep(bdrop));
}

TEST_CASE("M7.5: wake_body and disabling sleeping both reactivate a sleeper") {
    physics::PhysicsWorld w;
    add_ground(w);
    const physics::BodyId b = add_dynamic(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.5f, 0.0f});
    for (int i = 0; i < 200 && !w.is_asleep(b); ++i) {
        w.step(kDt);
    }
    REQUIRE(w.is_asleep(b));

    SUBCASE("wake_body reactivates it, and it later re-sleeps") {
        w.wake_body(b);
        CHECK(!w.is_asleep(b));
        bool re_slept = false;
        for (int i = 0; i < 200 && !re_slept; ++i) {
            w.step(kDt);
            re_slept = w.is_asleep(b);
        }
        CHECK(re_slept);
    }

    SUBCASE("disabling sleeping wakes it immediately and keeps it awake") {
        w.set_sleeping_enabled(false);
        CHECK(!w.is_asleep(b));
        for (int i = 0; i < 200; ++i) {
            w.step(kDt);
            CHECK(!w.is_asleep(b));
        }
    }
}

TEST_CASE("M7.5: the parallel step is bit-identical across worker counts (sleeping off)") {
    // The determinism contract. Sleeping is OFF so every island stays active every tick — the
    // parallel region does full solver work on all 300 ticks, which is the hard case to get
    // thread-count-invariant. workers < 0 means "no job system" (the sequential reference path).
    auto run_hash = [](int workers) -> std::uint64_t {
        physics::PhysicsWorld w;
        w.set_sleeping_enabled(false);
        build_multi_island_scene(w);
        std::unique_ptr<core::JobSystem> js;
        if (workers >= 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        for (int i = 0; i < 300; ++i) {
            w.step(kDt);
        }
        // Confirm the parallel path was actually exercised (more than one island to hand out).
        CHECK(w.islands_last() > 1);
        return w.world_hash();
    };

    const std::uint64_t sequential = run_hash(-1);
    CHECK(run_hash(1) == sequential);
    CHECK(run_hash(2) == sequential);
    CHECK(run_hash(3) == sequential);
    CHECK(run_hash(4) == sequential);
}

TEST_CASE("M7.5: the full pipeline (islands + sleeping) is bit-identical across worker counts") {
    // Same proof with sleeping ON — the deactivation bookkeeping runs sequentially between the
    // parallel regions, so it too must leave the cross-thread hash untouched.
    auto run_hash = [](int workers) -> std::uint64_t {
        physics::PhysicsWorld w; // sleeping on by default
        build_multi_island_scene(w);
        std::unique_ptr<core::JobSystem> js;
        if (workers >= 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        for (int i = 0; i < 300; ++i) {
            w.step(kDt);
        }
        return w.world_hash();
    };

    const std::uint64_t sequential = run_hash(-1);
    CHECK(run_hash(1) == sequential);
    CHECK(run_hash(2) == sequential);
    CHECK(run_hash(4) == sequential);
}
