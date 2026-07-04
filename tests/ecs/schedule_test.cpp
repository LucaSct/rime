// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.4b (the system scheduler). A Schedule batches Systems into parallel phases from
// their declared read/write access sets: independent systems collapse into one phase and run
// concurrently, while conflicting systems fall into successive phases that preserve their declared
// order. We prove the conflict rule, the phase leveling, and — the payoff — that independent
// systems run side by side over a *shared* archetype race-free (each writes a different column),
// while a reader system still sees an earlier writer's output. The concurrent path makes this a
// suite the TSan CI job runs.
//
// doctest macros stay on the main thread: Schedule::run joins each phase before returning, and the
// system bodies touch only disjoint component columns, so every CHECK runs after the joins.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/core/jobs.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/schedule.hpp"
#include "rime/ecs/system.hpp"
#include "rime/ecs/world.hpp"

using namespace rime::ecs;
using rime::core::JobSystem;

namespace st {
struct Pos {
    float x;
};

struct Vel {
    float dx;
};

struct Health {
    float hp;
};

struct Regen {
    float r;
};

struct Out {
    float v;
};
} // namespace st

// ---- the conflict rule ----

TEST_CASE("SystemAccess conflicts only when a write meets another access") {
    World w;
    const SystemAccess writes_pos{{}, signature_of<st::Pos>(w)};
    const SystemAccess reads_pos{signature_of<st::Pos>(w), {}};
    const SystemAccess writes_health{{}, signature_of<st::Health>(w)};

    CHECK(writes_pos.conflicts_with(writes_pos));          // write-write
    CHECK(writes_pos.conflicts_with(reads_pos));           // write-read
    CHECK(reads_pos.conflicts_with(writes_pos));           // read-write (symmetric)
    CHECK_FALSE(reads_pos.conflicts_with(reads_pos));      // read-read is safe
    CHECK_FALSE(writes_pos.conflicts_with(writes_health)); // disjoint components
}

// ---- phase leveling (no execution) ----

TEST_CASE("independent systems collapse into a single parallel phase") {
    World w;
    Schedule s;
    s.add({"a", {{}, signature_of<st::Pos>(w)}, [](World&, JobSystem&) {}});
    s.add({"b", {{}, signature_of<st::Health>(w)}, [](World&, JobSystem&) {}});
    s.add({"c", {signature_of<st::Vel>(w), {}}, [](World&, JobSystem&) {}});
    s.rebuild();

    CHECK(s.phase_count() == 1);
    CHECK(s.phases()[0].size() == 3);
}

TEST_CASE("a conflict chain serializes into one phase per link, in declared order") {
    World w;
    Schedule s;
    s.add({"write_pos", {{}, signature_of<st::Pos>(w)}, [](World&, JobSystem&) {}});
    s.add({"pos_to_out",
           {signature_of<st::Pos>(w), signature_of<st::Out>(w)},
           [](World&, JobSystem&) {}});
    s.add({"read_out", {signature_of<st::Out>(w), {}}, [](World&, JobSystem&) {}});
    s.rebuild();

    REQUIRE(s.phase_count() == 3);
    CHECK(s.phases()[0][0] == 0);
    CHECK(s.phases()[1][0] == 1);
    CHECK(s.phases()[2][0] == 2);
}

TEST_CASE("a system joins the phase after the last earlier system it conflicts with") {
    World w;
    Schedule s;
    s.add({"write_pos", {{}, signature_of<st::Pos>(w)}, [](World&, JobSystem&) {}}); // 0
    s.add(
        {"write_health", {{}, signature_of<st::Health>(w)}, [](World&, JobSystem&) {}}); // 1: indep
    s.add({"read_pos", {signature_of<st::Pos>(w), {}}, [](World&, JobSystem&) {}});      // 2: ⨯ 0
    s.rebuild();

    REQUIRE(s.phase_count() == 2);
    CHECK(s.phases()[0].size() == 2); // write_pos + write_health run together
    CHECK(s.phases()[1].size() == 1);
    CHECK(s.phases()[1][0] == 2); // read_pos waits for write_pos
}

TEST_CASE("two writers to the same component are serialized") {
    World w;
    Schedule s;
    s.add({"w1", {{}, signature_of<st::Pos>(w)}, [](World&, JobSystem&) {}});
    s.add({"w2", {{}, signature_of<st::Pos>(w)}, [](World&, JobSystem&) {}});
    s.rebuild();

    CHECK(s.phase_count() == 2);
}

// ---- concurrent execution ----

TEST_CASE("independent systems run concurrently over a shared archetype, race-free and correct") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 20000; // {Pos,Vel,Health,Regen} spans many chunks

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(st::Pos{static_cast<float>(i)},
                                  st::Vel{1.0f},
                                  st::Health{static_cast<float>(i)},
                                  st::Regen{2.0f}));
    }

    Schedule s;
    // move writes Pos (reads Vel); heal writes Health (reads Regen). Disjoint => one phase, run
    // side by side — writing DIFFERENT columns of the SAME chunks concurrently. This is the race
    // the access sets are meant to make impossible; TSan watches it.
    s.add({"move",
           {signature_of<st::Vel>(w), signature_of<st::Pos>(w)},
           [](World& world, JobSystem& j) {
               world.query<st::Pos, st::Vel>().par_for_each(
                   j, [](st::Pos& p, st::Vel& v) { p.x += v.dx; });
           }});
    s.add({"heal",
           {signature_of<st::Regen>(w), signature_of<st::Health>(w)},
           [](World& world, JobSystem& j) {
               world.query<st::Health, st::Regen>().par_for_each(
                   j, [](st::Health& h, st::Regen& g) { h.hp += g.r; });
           }});
    s.run(w, jobs);

    CHECK(s.phase_count() == 1); // both systems scheduled together
    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        ok = ok && (w.get<st::Pos>(es[i])->x == static_cast<float>(i) + 1.0f);
        ok = ok && (w.get<st::Health>(es[i])->hp == static_cast<float>(i) + 2.0f);
    }
    CHECK(ok);
}

TEST_CASE("a reader system sees an earlier writer system's output (phase ordering)") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 4000;

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(st::Pos{0.0f}, st::Out{-1.0f}));
    }

    Schedule s;
    // set_pos writes Pos = 5; copy reads Pos and writes Out. copy conflicts set_pos (it reads what
    // set_pos writes), so it runs in a later phase and must observe 5. Concurrent/reversed
    // execution would leave Out at its initial value.
    s.add({"set_pos", {{}, signature_of<st::Pos>(w)}, [](World& world, JobSystem& j) {
               world.query<st::Pos>().par_for_each(j, [](st::Pos& p) { p.x = 5.0f; });
           }});
    s.add({"copy",
           {signature_of<st::Pos>(w), signature_of<st::Out>(w)},
           [](World& world, JobSystem& j) {
               world.query<st::Pos, st::Out>().par_for_each(
                   j, [](st::Pos& p, st::Out& o) { o.v = p.x; });
           }});
    s.run(w, jobs);

    CHECK(s.phase_count() == 2);
    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        ok = ok && (w.get<st::Out>(es[i])->v == 5.0f);
    }
    CHECK(ok);
}

TEST_CASE("an empty schedule runs cleanly and has no phases") {
    World w;
    JobSystem jobs;
    Schedule s;
    s.run(w, jobs); // no-op, must not crash
    CHECK(s.system_count() == 0);
    CHECK(s.phase_count() == 0);
}

TEST_CASE("a single data-parallel system runs via the inline path") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 8000;

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(st::Pos{static_cast<float>(i)}, st::Vel{1.0f}));
    }

    Schedule s;
    s.add({"move",
           {signature_of<st::Vel>(w), signature_of<st::Pos>(w)},
           [](World& world, JobSystem& j) {
               world.query<st::Pos, st::Vel>().par_for_each(
                   j, [](st::Pos& p, st::Vel& v) { p.x += v.dx; });
           }});
    s.run(w, jobs);

    CHECK(s.phase_count() == 1);
    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        ok = ok && (w.get<st::Pos>(es[i])->x == static_cast<float>(i) + 1.0f);
    }
    CHECK(ok);
}

TEST_CASE("rebuild is idempotent and run repeats deterministically") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 3000;

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(st::Pos{0.0f}, st::Vel{1.0f}));
    }

    Schedule s;
    s.add({"move",
           {signature_of<st::Vel>(w), signature_of<st::Pos>(w)},
           [](World& world, JobSystem& j) {
               world.query<st::Pos, st::Vel>().par_for_each(
                   j, [](st::Pos& p, st::Vel& v) { p.x += v.dx; });
           }});

    s.rebuild();
    const auto plan = s.phases();
    s.rebuild();
    CHECK(s.phases() == plan); // rebuild is a pure function of the system list

    s.run(w, jobs); // +1
    s.run(w, jobs); // +1 again
    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        ok = ok && (w.get<st::Pos>(es[i])->x == 2.0f);
    }
    CHECK(ok);
}
