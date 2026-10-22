// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
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

// Fold a WorldStats into a running FNV-1a hash (core/hash.hpp) by its 14 DETERMINISTIC fields,
// packed into an array so the hash never depends on struct padding — the same discipline
// world_hash() uses.
//
// islands_solved_parallel is deliberately NOT in here, and it is the one field that must not be:
// it records how the tick was EXECUTED, not what the tick computed, so it is 0 on the sequential
// reference and the island count at every worker count. Hashing it would fail the determinism
// proof below for the one reason that is not a determinism failure — and the tempting repair,
// deleting the field to make the hash green, would delete the only witness that the parallel path
// ran at all.
std::uint64_t hash_stats(const physics::WorldStats& s, std::uint64_t seed) {
    const std::array<std::uint32_t, 14> f = {s.body_count,
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
                                             s.largest_island,
                                             s.largest_active_island};
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
    // The solved tail is a subset of the biggest island, and vanishes when nothing is solved.
    CHECK(s.largest_active_island <= s.largest_island);
    CHECK((s.active_islands > 0) == (s.largest_active_island > 0));
    // Bounded by the ACTIVE count, not by the island count — `<= islands` would be tautological
    // (the dispatch can only hand out islands that exist) and would pass for a counter that
    // reported fifty sleeping piles as fifty islands solved in parallel.
    CHECK(s.islands_solved_parallel <= s.active_islands);
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

// ── m17.5: the two things a report needs before it may conclude anything about parallelism ──

TEST_CASE("m17.5: the serial tail is the largest SOLVED island, not the largest one") {
    physics::PhysicsWorld w;
    constexpr int kStacks = 4;
    constexpr int kHeight = 5;
    build_pile(w, kStacks, kHeight);
    for (int i = 0; i < 400; ++i) {
        w.step(kDt); // long enough for every column to settle and fall asleep
    }
    const physics::WorldStats s = w.stats();

    // Everything is at rest. The islands are still THERE — the resting contacts persist, which is
    // what lets a faller wake the pile — but stage 6 solves none of them, so the solver's critical
    // path this tick is zero work. largest_island still reports the biggest pile, which is the
    // trap: read as "the serial tail" it over-states the critical path by the whole sleeping world,
    // and a load balancer told to optimise it would be chasing bodies that cost nothing.
    REQUIRE(s.active_islands == 0);
    CHECK(s.largest_island == static_cast<std::uint32_t>(kHeight));
    CHECK(s.largest_active_island == 0);
}

TEST_CASE("m17.5: islands_solved_parallel records the HANDOFF, not the pool") {
    // Two worlds identical but for one call. The point of the field is that NOTHING ELSE in the
    // stats can tell them apart — the parallel solve is bit-identical by contract (ADR-0026) — so
    // without it a report cannot distinguish "parallelism did not help" from "parallelism was
    // never switched on". 99-the-block was in the second state for its whole life while its ledger,
    // which recorded the job system's worker count, made it look like the first.
    auto measure = [](core::JobSystem* jobs) {
        physics::PhysicsWorld w;
        w.set_sleeping_enabled(false); // keep every island live, so every tick has work to hand out
        w.set_job_system(jobs);
        build_pile(w, /*stacks=*/4, /*height=*/3);
        for (int i = 0; i < 120; ++i) {
            w.step(kDt);
        }
        return w.stats();
    };
    core::JobSystem jobs{4};
    const physics::WorldStats seq = measure(nullptr);
    const physics::WorldStats par = measure(&jobs);

    REQUIRE(par.active_islands > 1); // the dispatch declines a tick with no width; this has four
    CHECK(seq.islands_solved_parallel == 0);
    CHECK(par.islands_solved_parallel == par.active_islands);

    // …and the two are otherwise the same tick, which is exactly why the field has to exist.
    CHECK(seq.islands == par.islands);
    CHECK(seq.active_islands == par.active_islands);
    CHECK(seq.largest_active_island == par.largest_active_island);
    CHECK(seq.contact_points == par.contact_points);
    CHECK(hash_stats(seq, core::kFnv1a64OffsetBasis) == hash_stats(par, core::kFnv1a64OffsetBasis));
}

TEST_CASE("m17.5: the tail is the largest island that was AWAKE, not the largest one present") {
    // The case the two tests above cannot see, and the one the field exists for: a big resting pile
    // beside a small moving thing. With everything asleep both numbers are 0, and with sleeping off
    // they are equal — so either alone is satisfied by `active_islands ? largest_island : 0`, which
    // is precisely the wrong answer here.
    physics::PhysicsWorld w;
    core::JobSystem jobs{4};
    w.set_job_system(&jobs);
    constexpr int kHeight = 5;
    build_pile(w, /*stacks=*/1, kHeight);
    for (int i = 0; i < 400; ++i) {
        w.step(kDt); // the column settles and the whole island falls asleep
    }
    REQUIRE(w.stats().active_islands == 0);

    // One box dropped four columns away — its own island, and the only one being solved.
    add_body(w, box({0.5f, 0.5f, 0.5f}), {16.0f, 4.0f, 0.0f});
    w.step(kDt);

    const physics::WorldStats s = w.stats();
    CHECK(s.islands == 2);
    CHECK(s.active_islands == 1);
    CHECK(s.largest_island == static_cast<std::uint32_t>(kHeight)); // the sleeping column
    CHECK(s.largest_active_island == 1);                            // the faller, and only it

    // …and with one island of real work and one of none, there is nothing to divide. Handing the
    // pair to the job system would pay a full wake-and-join to run one island on one worker, so
    // the dispatch declines and says so — the counter reports what was SOLVED in parallel, which
    // here is nothing, even though a job system is attached and two islands exist.
    CHECK(s.islands_solved_parallel == 0);
}

// ---- Island solve COST in time (the per-island profile) -----------------------------------------
//
// WorldStats::largest_active_island is a BODY COUNT. "92 % of awake bodies sit in one island, so
// the Amdahl ceiling is 1.09x" is a claim about WIDTH; body share is not solver-time share. This
// test builds one large island beside several small ones, with a DIFFERENT size for each so the
// cost-vs-size relationship has more than two points, and PRINTS what each island's solve costs.
// It asserts only what must be true (accounting, positivity, determinism) and never a magnitude:
// the nanoseconds are the measurement, and a machine-dependent number in an assertion is a flake.
namespace {

// Vertical columns of unit boxes, one per entry of `heights`, 4 m apart along x (never touching),
// each its own island. Returns the total body count.
int build_columns(physics::PhysicsWorld& w, const std::vector<int>& heights) {
    add_floor(w);
    int made = 0;
    for (std::size_t c = 0; c < heights.size(); ++c) {
        const float cx = static_cast<float>(c) * 4.0f;
        for (int j = 0; j < heights[c]; ++j) {
            add_body(w, box({0.5f, 0.5f, 0.5f}), {cx, 0.5f + static_cast<float>(j) * 1.02f, 0.0f});
            ++made;
        }
    }
    return made;
}

const std::vector<int> kColumnHeights = {24, 12, 6, 3, 2, 2};

} // namespace

TEST_CASE("island profile: per-island solve cost, serial fraction, and a hash that cannot tell") {
    constexpr int kWarmup = 60;
    constexpr int kMeasured = 120;
    core::JobSystem jobs{4};

    physics::PhysicsWorld on;  // profiled, sequential solve: clean per-island times
    physics::PhysicsWorld off; // the same scene, profile never enabled
    physics::PhysicsWorld par; // profiled, island solve on the job system
    for (physics::PhysicsWorld* w : {&on, &off, &par}) {
        w->set_sleeping_enabled(false); // keep every island active so every step has work to time
    }
    par.set_job_system(&jobs);
    const int made = build_columns(on, kColumnHeights);
    build_columns(off, kColumnHeights);
    build_columns(par, kColumnHeights);
    REQUIRE(!on.island_profile_enabled()); // OFF by default
    CHECK(on.island_solve_profile_last().empty());
    on.set_island_profile_enabled(true);
    par.set_island_profile_enabled(true);

    struct Acc {
        std::uint32_t bodies = 0;
        std::uint32_t constraints = 0;
        std::uint64_t ns_sum = 0;
        std::array<std::uint64_t, 4> phase_ns{};
        std::array<int, 4> zero_samples{};
        int steps = 0;
    };

    std::map<std::uint32_t, Acc> acc;
    std::map<std::uint32_t, Acc> parallel_acc;
    const auto has_phase_evidence = [](const Acc& a) {
        return std::all_of(
            a.phase_ns.begin(), a.phase_ns.end(), [](std::uint64_t ns) { return ns > 0; });
    };
    std::uint64_t total_sum = 0;
    std::uint64_t largest_sum = 0;
    int measured = 0;
    int changed_steady = 0;
    // Phase split of the biggest island (24 stacked boxes), summed over the measured steps.
    std::uint64_t big_velocity_sum = 0;
    std::uint64_t big_store_sum = 0;
    std::uint64_t big_solve_sum = 0;
    std::uint64_t big_phase_sum = 0;

    for (int i = 0; i < kWarmup + kMeasured; ++i) {
        on.step(kDt);
        off.step(kDt);
        par.step(kDt);
        const physics::WorldStats s = on.stats();
        const auto prof = on.island_solve_profile_last();

        // Accounting that must hold every step, whatever the clock says.
        REQUIRE(prof.size() == s.active_islands);
        std::uint32_t body_sum = 0;
        std::uint32_t most_bodies = 0;
        std::uint64_t ns_sum = 0;
        std::uint64_t ns_max = 0;
        for (const physics::IslandSolveCost& c : prof) {
            body_sum += c.bodies;
            most_bodies = std::max(most_bodies, c.bodies);
            ns_sum += c.solve_ns;
            ns_max = std::max(ns_max, c.solve_ns);
        }
        CHECK(body_sum ==
              s.awake_bodies); // sleeping is off: every dynamic body is awake and active
        CHECK(body_sum == static_cast<std::uint32_t>(made));
        CHECK(most_bodies == s.largest_active_island);
        CHECK(s.island_solve_ns_total == ns_sum);
        CHECK(s.largest_active_island_solve_ns == ns_max);
        CHECK(s.island_solve_ns_total >= s.largest_active_island_solve_ns);
        CHECK(s.island_solve_ns_total > 0);
        CHECK(s.largest_active_island_solve_ns > 0);
        // The profiled worker-thread run reports the same island STRUCTURE (times differ).
        const auto pprof = par.island_solve_profile_last();
        REQUIRE(pprof.size() == prof.size());
        for (std::size_t k = 0; k < prof.size(); ++k) {
            CHECK(pprof[k].island == prof[k].island);
            CHECK(pprof[k].bodies == prof[k].bodies);
            CHECK(pprof[k].constraints == prof[k].constraints);
        }
        // The four phase buckets (warm / velocity / store / position). Every island here has
        // contacts (each column rests on the floor), so every bucket must have read a clock.
        // Reading a monotonic clock does not guarantee it advanced: Windows reported zero for
        // individual impulse-store laps even though their cumulative time was positive. Accept
        // sub-tick samples here and require evidence for every island/phase over the measured
        // window below, for both sequential and worker-thread solves.
        // The buckets are laps of ONE stopwatch sequence inside the island's own whole-island
        // stopwatch, so they cannot sum to more than solve_ns -- but a strict `<=` is not safe:
        // each bucket is rounded to the nearest ns on its own (up to +0.5 ns apiece), and the
        // double-microsecond clock read can disagree with its neighbour by a few ns. The slack is
        // 5 % of solve_ns plus 100 ns. This catches substantial overlap; duplication of a small
        // bucket such as store can fit inside the slack and is not proven absent by this bound.
        for (const auto costs : {prof, pprof}) {
            for (const physics::IslandSolveCost& c : costs) {
                if (c.constraints == 0) {
                    continue;
                }
                const std::uint64_t phases = c.warm_ns + c.velocity_ns + c.store_ns + c.position_ns;
                CHECK(phases <= c.solve_ns + c.solve_ns / 20 + 100);
            }
        }
        // Profile off means all of it is zero -- and the phase fields are covered by the
        // emptiness check below, NOT by a loop asserting they are zero. Profile-off publishes no
        // entries at all, so such a loop runs zero times and reports PASS having inspected
        // nothing, which is the house failure mode (a proof that cannot see what it skipped still
        // reads as passing). Emptiness is the claim that can actually fail.
        const physics::WorldStats so = off.stats();
        CHECK(off.island_solve_profile_last().empty());
        CHECK(so.island_solve_ns_total == 0);
        CHECK(so.largest_active_island_solve_ns == 0);

        if (i >= kWarmup) {
            ++measured;
            total_sum += s.island_solve_ns_total;
            largest_sum += s.largest_active_island_solve_ns;
            changed_steady += s.largest_island_membership_changed ? 1 : 0;
            const auto accumulate = [&](std::map<std::uint32_t, Acc>& totals,
                                        const physics::IslandSolveCost& c) {
                // Island numbers are partition indices, not persistent IDs. This particular
                // steady fixture must retain its six separate columns throughout measurement.
                REQUIRE(c.island < kColumnHeights.size());
                CHECK(c.bodies == static_cast<std::uint32_t>(kColumnHeights[c.island]));
                REQUIRE(c.constraints > 0);
                Acc& a = totals[c.island];
                a.bodies = c.bodies;
                a.constraints = c.constraints;
                a.ns_sum += c.solve_ns;
                const std::array phases{c.warm_ns, c.velocity_ns, c.store_ns, c.position_ns};
                for (std::size_t phase = 0; phase < phases.size(); ++phase) {
                    a.phase_ns[phase] += phases[phase];
                    a.zero_samples[phase] += phases[phase] == 0 ? 1 : 0;
                }
                ++a.steps;
            };
            REQUIRE(prof.size() == kColumnHeights.size());
            for (const physics::IslandSolveCost& c : prof) {
                accumulate(acc, c);
                if (c.bodies == static_cast<std::uint32_t>(kColumnHeights[0])) {
                    big_velocity_sum += c.velocity_ns;
                    big_store_sum += c.store_ns;
                    big_solve_sum += c.solve_ns;
                    big_phase_sum += c.warm_ns + c.velocity_ns + c.store_ns + c.position_ns;
                }
            }
            for (const physics::IslandSolveCost& c : pprof) {
                accumulate(parallel_acc, c);
            }
        }
    }

    // THE determinism check: timing is a read-only side channel, so the profile must be invisible
    // to the simulation — on, off, and on-with-workers all land on the same bits.
    CHECK(on.world_hash() == off.world_hash());
    CHECK(par.world_hash() == off.world_hash());

    // A missing phase timer must still fail. Positivity belongs to the whole measured window,
    // separately for each island and phase, rather than to a single clock tick. These controls
    // erase each phase in turn from otherwise real measurements and use the SAME predicate,
    // proving that legitimate zero samples have not made absent instrumentation invisible.
    constexpr std::array phase_names{"warm", "velocity", "store", "position"};
    for (const auto* totals : {&acc, &parallel_acc}) {
        REQUIRE(totals->size() == kColumnHeights.size());
        for (const auto& [id, a] : *totals) {
            CAPTURE(id);
            CAPTURE(totals == &parallel_acc);
            CAPTURE(a.steps);
            CHECK(a.steps == kMeasured);
            INFO("phase totals (warm/velocity/store/position): ",
                 a.phase_ns[0],
                 "/",
                 a.phase_ns[1],
                 "/",
                 a.phase_ns[2],
                 "/",
                 a.phase_ns[3]);
            INFO("zero samples: ",
                 a.zero_samples[0],
                 "/",
                 a.zero_samples[1],
                 "/",
                 a.zero_samples[2],
                 "/",
                 a.zero_samples[3]);
            CHECK(has_phase_evidence(a));
            for (std::size_t phase = 0; phase < phase_names.size(); ++phase) {
                CAPTURE(phase_names[phase]);
                Acc missing = a;
                missing.phase_ns[phase] = 0;
                CHECK_FALSE(has_phase_evidence(missing));
            }
        }
    }

    // FALSIFICATION that the instrument can see a difference at all: the velocity phase runs 8
    // iterations over the island's constraints, store_impulses is one pass over them, so velocity
    // must dominate store. Summed over the measured steps so one preempted step cannot flip it.
    // A near-equal pair would mean the buckets are not reading different work.
    CHECK(big_velocity_sum > big_store_sum);
    std::printf("[island-profile] 24-box island, mean/step: solve %.0f ns, velocity %.0f ns, store "
                "%.0f ns, sum of phases %.0f ns\n",
                static_cast<double>(big_solve_sum) / measured,
                static_cast<double>(big_velocity_sum) / measured,
                static_cast<double>(big_store_sum) / measured,
                static_cast<double>(big_phase_sum) / measured);

    std::printf(
        "\n[island-profile] %d bodies, %zu columns, mean over %d steps (sequential solve)\n",
        made,
        kColumnHeights.size(),
        measured);
    std::printf("[island-profile] %8s %8s %12s %12s %14s\n",
                "island",
                "bodies",
                "constraints",
                "solve_ns",
                "ns_per_body");
    double largest_by_time_ns = 0.0;
    double total_ns = 0.0;
    for (const auto& [id, a] : acc) {
        const double mean = static_cast<double>(a.ns_sum) / std::max(1, a.steps);
        total_ns += mean;
        largest_by_time_ns = std::max(largest_by_time_ns, mean);
        std::printf("[island-profile] %8u %8u %12u %12.0f %14.0f\n",
                    id,
                    a.bodies,
                    a.constraints,
                    mean,
                    mean / static_cast<double>(std::max(1u, a.bodies)));
    }
    const double frac = largest_by_time_ns / total_ns;
    std::printf("[island-profile] serial fraction (largest island time / total) = %.4f\n", frac);
    std::printf("[island-profile] implied island-parallel ceiling (1 / fraction) = %.3fx\n",
                1.0 / frac);
    std::printf("[island-profile] per-step means: total %.0f ns, largest %.0f ns\n",
                static_cast<double>(total_sum) / measured,
                static_cast<double>(largest_sum) / measured);
    std::printf("[island-profile] largest-island membership changed on %d of %d steady steps\n",
                changed_steady,
                measured);

    // Merge event: a box set down overlapping the foot of the biggest column joins that island. The
    // signal must see it (membership changed, previous count one smaller).
    const std::uint32_t before = on.stats().largest_active_island;
    add_body(on, box({0.5f, 0.5f, 0.5f}), {0.95f, 0.5f, 0.0f});
    on.step(kDt);
    const physics::WorldStats after = on.stats();
    std::printf(
        "[island-profile] merge step: largest %u -> %u bodies, changed=%d, prev_bodies=%u\n",
        before,
        after.largest_active_island,
        after.largest_island_membership_changed ? 1 : 0,
        after.largest_island_prev_bodies);
    CHECK(after.largest_active_island == before + 1);
    CHECK(after.largest_island_membership_changed);
    CHECK(after.largest_island_prev_bodies == before);
}

// ---- Wavefront velocity sweep: a cost knob that must not be visible in the bits -----------------
//
// The wavefront path (engine/physics/src/wavefront.hpp) reorders a large island's Gauss–Seidel
// sweep into dependency LEVELS — constraints at the same level share no dynamic body — and solves
// up to four of them at once in SSE2. Reordering is only legal because every constraint still sees
// exactly the body velocities the scalar sweep would have handed it, and the lanes do the same
// float operations in the same order. So the claim is not "close": it is the SAME world_hash on
// every step. A rounding drift would show here as a hash split within a handful of steps, because
// a collapsing pile amplifies the last bit (the chaotic-cascade argument in CLAUDE.md).
//
// The non-vacuousness half matters as much as the equality. Two scalar worlds also agree to the
// bit, so a hash match proves nothing unless the wide path actually RAN: wavefront_islands_last()
// counts the islands that took it, and the pile is built to clear the 32-constraint threshold.
namespace {

// A brick-bonded heap of unit boxes: kSide x kSide per layer, alternate layers shifted half a box
// so every layer leans on two below it, all touching laterally — ONE island of well over 32
// contact constraints. A box thrown in from the side knocks it over, so the scene exercises a
// changing contact graph (new manifolds, broken ones, island churn), not only a resting stack.
void build_heap(physics::PhysicsWorld& w) {
    constexpr int kSide = 4;
    constexpr int kLayers = 4;
    add_floor(w);
    for (int l = 0; l < kLayers; ++l) {
        const float shift = (l % 2 == 0) ? 0.0f : 0.5f;
        for (int x = 0; x < kSide; ++x) {
            for (int z = 0; z < kSide; ++z) {
                add_body(w,
                         box({0.5f, 0.5f, 0.5f}),
                         {static_cast<float>(x) * 1.01f + shift,
                          0.5f + static_cast<float>(l) * 1.02f,
                          static_cast<float>(z) * 1.01f + shift});
            }
        }
    }
    physics::BodyDesc ram;
    ram.motion = physics::MotionType::Dynamic;
    ram.shape = box({0.6f, 0.6f, 0.6f});
    ram.position = {-6.0f, 3.0f, 1.5f};
    ram.linear_velocity = {14.0f, 0.0f, 0.0f};
    static_cast<void>(w.create_body(ram));
}

} // namespace

TEST_CASE("wavefront velocity sweep is bit-identical to the scalar sweep, and actually runs") {
    constexpr int kSteps = 120;
    physics::PhysicsWorld off;
    physics::PhysicsWorld on;
    for (physics::PhysicsWorld* w : {&off, &on}) {
        w->set_sleeping_enabled(false); // keep the pile solving every step
        build_heap(*w);
    }
    off.set_wavefront_solve_enabled(false);
    on.set_wavefront_solve_enabled(true);
    CHECK_FALSE(off.wavefront_solve_enabled());

    int wide_steps = 0;
    for (int i = 0; i < kSteps; ++i) {
        off.step(kDt);
        on.step(kDt);
        CAPTURE(i);
        // The scalar world never takes the wide path, whatever the island sizes.
        CHECK(off.wavefront_islands_last() == 0);
        wide_steps += on.wavefront_islands_last() > 0 ? 1 : 0;
        // EVERY step, not just the last: a mid-run divergence that later re-converged (or a
        // single-step mismatch hidden by a final hash) would otherwise read as passing.
        REQUIRE(on.world_hash() == off.world_hash());
    }

    // On x86_64 the wide sweep is compiled in; elsewhere (arm64 keeps the scalar sweep until a
    // NEON port is checked against that compiler's contraction) the request is refused and the
    // equality above is a scalar-vs-scalar check, which is all that platform can claim.
    if (on.wavefront_solve_enabled()) {
        // The heap is one island of > 32 contact constraints from the first steps the boxes touch,
        // and sleeping is off, so all but the opening contact-forming steps must go wide. Printed
        // so a reader sees the coverage, not just a pass.
        std::printf("[wavefront] wide path ran on %d of %d steps\n", wide_steps, kSteps);
        CHECK(wide_steps >= kSteps - 10);
    } else {
        CHECK(wide_steps == 0);
    }
}
