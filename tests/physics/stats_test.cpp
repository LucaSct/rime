// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "rime/core/hash.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.13 proofs: PhysicsWorld::stats() is the simulation's instrument panel (WorldStats) — a
// deterministic per-tick COUNT of body population, collision load, and island structure, the
// measurement half of the debris-scale capstone. The checks are structural in the house pattern:
//   * accounting — the motion-type counts partition the bodies, awake+sleeping == dynamic;
//   * convergence — a dropped pile ends with every body asleep, active_islands == 0, yet the
//     settled contacts (manifolds/islands) are still reported (a resting pile isn't invisible);
//   * load — a stack of K reports one island of size K, resting contacts warm-start;
//   * agreement — stats() carries the same numbers as islands_last()/contacts_warm_started_last();
//   * the headline — the WHOLE stats stream is bit-identical across worker counts (the M7.5
//     determinism thesis, now over the stats too: they are a pure function of the tick, no clock).
// Everything drives the public seam and is pure-CPU, so it runs on every CI OS + the sanitizers.
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

physics::ShapeDesc box(core::Vec3 half) {
    return physics::ShapeDesc{physics::ShapeType::Box, 0.0f, half, 0.0f};
}

physics::BodyId add_body(physics::PhysicsWorld& w,
                         const physics::ShapeDesc& shape,
                         core::Vec3 pos,
                         physics::MotionType motion = physics::MotionType::Dynamic) {
    physics::BodyDesc d;
    d.motion = motion;
    d.shape = shape;
    d.position = pos;
    return w.create_body(d);
}

// A big static floor whose top surface sits at y = 0 (the pile lands on it).
physics::BodyId add_floor(physics::PhysicsWorld& w) {
    return add_body(w, box({50.0f, 0.5f, 50.0f}), {0.0f, -0.5f, 0.0f}, physics::MotionType::Static);
}

// `stacks` separated vertical columns of `height` unit boxes each — spaced far enough apart (4 m)
// that no two columns ever touch, so once settled the scene is `stacks` islands of `height` bodies:
// a controllable multi-island load for the parallel-solve determinism proof and the island stats.
void build_pile(physics::PhysicsWorld& w, int stacks, int height) {
    add_floor(w);
    const int side = static_cast<int>(std::sqrt(static_cast<double>(stacks)) + 0.999);
    int made = 0;
    for (int s = 0; s < stacks; ++s) {
        const float cx = static_cast<float>(s % side) * 4.0f;
        const float cz = static_cast<float>(s / side) * 4.0f;
        for (int j = 0; j < height; ++j) {
            // 1.02 vertical spacing: a hair above face contact so the column settles cleanly into a
            // touching stack (one island) rather than starting inter-penetrating.
            add_body(w, box({0.5f, 0.5f, 0.5f}), {cx, 0.5f + static_cast<float>(j) * 1.02f, cz});
            ++made;
        }
    }
    REQUIRE(made == stacks * height);
}

// Fold a WorldStats into a running FNV-1a hash (core/hash.hpp) by its 13 fields, packed into an
// array so the hash never depends on struct padding — the same discipline world_hash() uses.
std::uint64_t hash_stats(const physics::WorldStats& s, std::uint64_t seed) {
    const std::array<std::uint32_t, 13> f = {s.body_count,
                                             s.dynamic_bodies,
                                             s.static_bodies,
                                             s.kinematic_bodies,
                                             s.awake_bodies,
                                             s.sleeping_bodies,
                                             s.broadphase_pairs,
                                             s.manifolds,
                                             s.contact_points,
                                             s.contacts_warm_started,
                                             s.islands,
                                             s.active_islands,
                                             s.largest_island};
    return core::fnv1a_64(std::as_bytes(std::span<const std::uint32_t>{f}), seed);
}

// The invariants every tick must satisfy at any worker count — the counts partition the way the
// struct's contract promises. Called after each step in the determinism proof so a violation is
// caught in the act, not just at the end.
void check_invariants(const physics::WorldStats& s) {
    CHECK(s.dynamic_bodies + s.static_bodies + s.kinematic_bodies == s.body_count);
    CHECK(s.awake_bodies + s.sleeping_bodies == s.dynamic_bodies);
    CHECK(s.active_islands <= s.islands);
    CHECK(s.largest_island <= s.dynamic_bodies);
    CHECK(s.contact_points >= s.manifolds);   // every manifold carries at least one point
    CHECK(s.manifolds <= s.broadphase_pairs); // box-only scene: no compound pair fans out
    CHECK(s.contacts_warm_started <= s.contact_points);
}

} // namespace

TEST_CASE("M7.13: body population accounting partitions by motion type") {
    physics::PhysicsWorld w;
    w.set_sleeping_enabled(false); // hold everything awake so awake_bodies == dynamic_bodies

    // Fresh world, before the first step: the panel is all zeros (no tick has run).
    CHECK(w.stats().body_count == 0);
    CHECK(w.stats().dynamic_bodies == 0);

    constexpr int kBoxes = 12;
    build_pile(w, /*stacks=*/4, /*height=*/3); // 12 dynamic boxes + 1 floor

    w.step(kDt);
    const physics::WorldStats s = w.stats();
    CHECK(s.body_count == kBoxes + 1);
    CHECK(s.static_bodies == 1);
    CHECK(s.kinematic_bodies == 0);
    CHECK(s.dynamic_bodies == kBoxes);
    CHECK(s.dynamic_bodies + s.static_bodies + s.kinematic_bodies == s.body_count);
    // Sleeping is off, so every dynamic body is awake.
    CHECK(s.awake_bodies == kBoxes);
    CHECK(s.sleeping_bodies == 0);
}

TEST_CASE("M7.13: a dropped pile settles — sleeping shows in the panel, resting contacts persist") {
    physics::PhysicsWorld w; // sleeping ON by default
    constexpr int kStacks = 4;
    constexpr int kHeight = 3;
    constexpr int kBoxes = kStacks * kHeight;
    build_pile(w, kStacks, kHeight);

    for (int i = 0; i < 400; ++i) {
        w.step(kDt);
    }
    const physics::WorldStats s = w.stats();

    // Convergence: the whole pile is asleep, so nothing is active this tick — the M7.5 sleeping
    // payoff, now observable. awake_bodies == 0 is the cheap "is the scene at rest?" query.
    CHECK(s.awake_bodies == 0);
    CHECK(s.sleeping_bodies == kBoxes);
    CHECK(s.active_islands == 0);

    // But a settled pile is NOT invisible: broadphase + narrowphase still run over the asleep
    // bodies every tick, so the resting stack-and-floor contacts are still reported. This is what
    // lets a faller wake the pile (the contact is already there to merge on).
    CHECK(s.islands >= 1);
    CHECK(s.manifolds >= 1);
    CHECK(s.contact_points >= s.manifolds);
    // Four separated columns never touch each other ⇒ four resting islands.
    CHECK(s.islands == kStacks);
}

TEST_CASE("M7.13: a stack of K is one island of K; resting contacts warm-start") {
    physics::PhysicsWorld w;
    w.set_sleeping_enabled(false); // keep the stack live so it keeps solving + warm-starting
    constexpr int kHeight = 5;
    build_pile(w, /*stacks=*/1, kHeight);

    for (int i = 0; i < 200; ++i) {
        w.step(kDt);
    }
    const physics::WorldStats s = w.stats();

    // One column of touching boxes ⇒ a single island holding all K dynamic bodies (the static floor
    // is a shared anchor, not an island node — it never merges columns or inflates the size).
    CHECK(s.islands == 1);
    CHECK(s.largest_island == static_cast<std::uint32_t>(kHeight));

    // A resting stack warm-starts: this tick's contacts inherited last tick's solved impulses by
    // feature id. stats() must report exactly the standalone accessor's number.
    CHECK(s.contacts_warm_started > 0);
    CHECK(s.contacts_warm_started == w.contacts_warm_started_last());
    CHECK(s.contacts_warm_started <= s.contact_points);
}

TEST_CASE("M7.13: stats() agrees with the legacy single-value witnesses") {
    physics::PhysicsWorld w;
    build_pile(w, /*stacks=*/3, /*height=*/2);
    for (int i = 0; i < 5; ++i) {
        w.step(kDt);
    }
    CHECK(w.stats().islands == w.islands_last());
    CHECK(w.stats().contacts_warm_started == w.contacts_warm_started_last());
}

TEST_CASE("M7.13: the whole stats stream is bit-identical across worker counts") {
    // The headline. A debris pile stepped many ticks, hashing every tick's WorldStats into a
    // running fingerprint: because the stats derive only from the canonical manifolds and the
    // pure-function island partition (no clock, no thread-order dependence), the fingerprint must
    // match the sequential reference for 1, 2, and 4 workers — the M7.5 determinism contract
    // extended over the instrument panel. Sleeping ON so the (sequential) sleep bookkeeping between
    // the parallel regions is exercised too. workers < 0 is the no-job-system reference path.
    // static so the lambda can use them without capturing (portable across GCC/clang capture
    // rules).
    static constexpr int kStacks = 16;
    static constexpr int kHeight =
        8; // 128 dynamic bodies — genuinely multi-island, at modest scale
    static constexpr int kTicks = 200;

    auto run = [](int workers) -> std::uint64_t {
        physics::PhysicsWorld w;
        build_pile(w, kStacks, kHeight);
        std::unique_ptr<core::JobSystem> js;
        if (workers >= 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        std::uint64_t h = core::kFnv1a64OffsetBasis;
        for (int i = 0; i < kTicks; ++i) {
            w.step(kDt);
            const physics::WorldStats s = w.stats();
            check_invariants(s);
            h = hash_stats(s, h);
        }
        // The parallel path was genuinely exercised: many islands to hand out.
        CHECK(w.islands_last() == kStacks);
        return h;
    };

    const std::uint64_t sequential = run(-1);
    CHECK(run(1) == sequential);
    CHECK(run(2) == sequential);
    CHECK(run(4) == sequential);
}
