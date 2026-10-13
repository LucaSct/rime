// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>

#include "islands.hpp" // the sleep thresholds and the pose rest envelope they derive
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
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

// ── Tier-2 matrix validation: sequential vs parallel, WITH the sensitivity control ──
TEST_CASE("validate: tumbling elongated boxes in 16 islands - parallel hash == sequential hash") {
    // Why elongated AND tilted: the world inverse inertia is R * diag(1/Ixx,1/Iyy,1/Izz) * R^T.
    // That is a full (non-diagonal) matrix only when the three principal moments DIFFER (a long
    // thin box) and the body is rotated off the world axes (the tilt, then spin keeps it so). A
    // cube or sphere has isotropic inertia, I = k*Identity, so R * (1/k)I * R^T = (1/k)I stays
    // diagonal under every rotation: the cube/sphere scenes in build_multi_island_scene cannot
    // express any divergence in how the off-diagonal terms are stored or multiplied, so their
    // bit-identity proofs are structurally blind to it. This scene is the first that can. The eight
    // columns hold two boxes each and the pair never touches, so every BOX is its own island:
    // sixteen of them, measured, not the eight the column count suggests. The control is
    // islands_solved_parallel: it must exceed 1 in the parallel run and be 0 in the sequential one,
    // or the comparison proves nothing.
    struct R {
        std::uint64_t hash;
        std::uint32_t max_parallel;
        std::uint32_t active;
    };

    auto run = [](int workers) -> R {
        physics::PhysicsWorld w;
        w.set_sleeping_enabled(false);
        add_ground(w);
        for (int k = 0; k < 8; ++k) {
            for (int j = 0; j < 2; ++j) {
                physics::BodyDesc d;
                d.motion = physics::MotionType::Dynamic;
                d.shape = box({0.2f, 0.6f, 1.0f});
                d.position = {k * 6.0f - 21.0f, 1.5f + 2.5f * static_cast<float>(j), 0.0f};
                d.orientation = core::normalize(core::Quat{0.3f * static_cast<float>(k + 1),
                                                           0.2f,
                                                           0.1f * static_cast<float>(j + 1),
                                                           1.0f});
                d.angular_velocity = {1.0f, 0.5f * static_cast<float>(k), -2.0f};
                static_cast<void>(w.create_body(d));
            }
        }
        std::unique_ptr<core::JobSystem> js;
        if (workers >= 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        R r{0, 0, 0};
        for (int i = 0; i < 300; ++i) {
            w.step(kDt);
            const auto s = w.stats();
            r.max_parallel = std::max(r.max_parallel, s.islands_solved_parallel);
            r.active = std::max(r.active, s.active_islands);
        }
        r.hash = w.world_hash();
        return r;
    };
    const R seq = run(-1);
    MESSAGE("seq: hash=",
            seq.hash,
            " max_islands_solved_parallel=",
            seq.max_parallel,
            " max_active=",
            seq.active);
    CHECK(seq.max_parallel == 0);
    for (int workers : {1, 2, 4}) {
        const R par = run(workers);
        MESSAGE("workers=",
                workers,
                " hash=",
                par.hash,
                " max_islands_solved_parallel=",
                par.max_parallel,
                " max_active=",
                par.active);
        CHECK(par.max_parallel > 1);
        CHECK(par.hash == seq.hash);
    }
}

// ---------------------------------------------------------------- the POSE rest route (2026-10) --
// Why this route exists, measured on the destruction wall: the speed test is sampled once per step,
// so a chunk pinched in a settling pile that is kicked above the bound every few ticks while moving
// micrometres never accrues the half second — and because the island needs EVERY member
// simultaneously, one such chunk kept eighteen bodies awake for nine hundred ticks. The second
// route asks the question sleeping actually asserts: has the body GONE anywhere?
// Evidence: /home/next/reports/rime-sleep-detector-2026-10-13/REPORT.md.

TEST_CASE("the pose rest envelope is the speed thresholds, integrated") {
    // THE CONTROL, and the whole argument that this is not a loosening: the envelope is DERIVED
    // from the existing thresholds, so steady motion at exactly the speed bound covers exactly the
    // displacement bound. Were these ever tuned apart, the route would stop being "the same
    // tolerance, integrated" and start being a second, unargued knob.
    CHECK(physics::kPoseRestTranslation == physics::kLinearSleepThreshold * physics::kTimeToSleep);
    CHECK(physics::kPoseRestRotation == physics::kAngularSleepThreshold * physics::kTimeToSleep);

    const core::Vec3 anchor_p{3.0f, -1.0f, 0.5f};
    const core::Quat anchor_q =
        core::quat_from_axis_angle(core::normalize(core::Vec3{1.0f, 2.0f, -0.5f}), 0.7f);
    const float r = physics::kPoseRestTranslation;

    SUBCASE("translation is bounded by the envelope, on both sides of it") {
        for (const core::Vec3 dir : {core::Vec3{1.0f, 0.0f, 0.0f},
                                     core::Vec3{0.0f, 1.0f, 0.0f},
                                     core::Vec3{0.0f, 0.0f, 1.0f},
                                     core::normalize(core::Vec3{1.0f, 1.0f, 1.0f})}) {
            CHECK(physics::pose_within_rest_envelope(
                anchor_p + dir * (r * 0.95f), anchor_q, anchor_p, anchor_q));
            CHECK_FALSE(physics::pose_within_rest_envelope(
                anchor_p + dir * (r * 1.05f), anchor_q, anchor_p, anchor_q));
        }
    }

    SUBCASE("rotation is bounded by the envelope, about any axis") {
        const float a = physics::kPoseRestRotation;
        for (const core::Vec3 axis : {core::Vec3{1.0f, 0.0f, 0.0f},
                                      core::Vec3{0.0f, 0.0f, 1.0f},
                                      core::normalize(core::Vec3{-2.0f, 1.0f, 3.0f})}) {
            const core::Quat inside = anchor_q * core::quat_from_axis_angle(axis, a * 0.8f);
            const core::Quat outside = anchor_q * core::quat_from_axis_angle(axis, a * 1.2f);
            CHECK(physics::pose_within_rest_envelope(anchor_p, inside, anchor_p, anchor_q));
            CHECK_FALSE(physics::pose_within_rest_envelope(anchor_p, outside, anchor_p, anchor_q));
        }
    }

    SUBCASE("q and -q are the same orientation — the double cover must not deny sleep") {
        // A unit quaternion and its negation name the same rotation. Comparing them without the
        // absolute value reads as a rotation of nearly 2π, so a body that happened to drift across
        // the sign boundary would be refused sleep forever, at random. That failure would be
        // invisible in every test above, because they all stay on one side of it.
        const core::Quat negated{-anchor_q.x, -anchor_q.y, -anchor_q.z, -anchor_q.w};
        CHECK(physics::pose_within_rest_envelope(anchor_p, negated, anchor_p, anchor_q));
        CHECK(physics::pose_within_rest_envelope(anchor_p, anchor_q, anchor_p, negated));
    }
}

TEST_CASE("the pose route does not let a steadily DRIFTING body sleep") {
    // The control on the behaviour, not on the formula: the new route must not turn into "sleep
    // anything that is not obviously flying". A body translating just above the speed bound fails
    // the speed route, and must fail the pose route too — which it does only because the envelope
    // is the speed bound integrated. Both halves are checked, so neither route can be the one
    // carrying the result by accident.
    const float just_over = physics::kLinearSleepThreshold * 1.2f;
    const float just_under = physics::kLinearSleepThreshold * 0.8f;

    const auto drifting_body_sleeps_within = [](float speed, int budget) {
        physics::PhysicsWorld w;
        w.set_gravity({0.0f, 0.0f, 0.0f}); // no contacts, no gravity: a pure drift
        physics::BodyDesc d;
        d.motion = physics::MotionType::Dynamic;
        d.shape = box({0.5f, 0.5f, 0.5f});
        d.position = {0.0f, 0.0f, 0.0f};
        d.linear_velocity = {speed, 0.0f, 0.0f};
        const physics::BodyId b = w.create_body(d);
        for (int i = 0; i < budget; ++i) {
            w.step(kDt);
            if (w.is_asleep(b)) {
                return true;
            }
        }
        return false;
    };

    // 300 ticks is 5 s, ten times the half second either route needs.
    CHECK_FALSE(drifting_body_sleeps_within(just_over, 300));
    CHECK(drifting_body_sleeps_within(just_under, 300)); // the speed route still works
}
