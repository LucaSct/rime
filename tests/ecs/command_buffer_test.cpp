// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.4c (deferred structural changes). A CommandBuffer records spawn/despawn/add/remove
// and replays them at a safe point via apply(), so a system can restructure the world without
// mutating the archetypes it (or a concurrent system) is iterating. We prove: each recorded op
// takes effect on apply and NOT before; recording from inside par_for_each is thread-safe (many
// workers, one buffer); and the Schedule applies a system's commands at the phase boundary. The
// concurrent-recording case makes this a suite the TSan CI job runs.
//
// doctest macros stay on the main thread: apply() and the joins happen before every CHECK.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/jobs.hpp"
#include "rime/ecs/command_buffer.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/schedule.hpp"
#include "rime/ecs/system.hpp"
#include "rime/ecs/world.hpp"

using namespace rime::ecs;
using rime::core::JobSystem;

namespace cbt {
struct Pos {
    float x;
};

struct Vel {
    float dx;
};

struct Doomed {
    std::int32_t v; // 1 => mark for despawn
};

struct Added {
    float a;
};
} // namespace cbt

TEST_CASE("recorded spawns take effect on apply, not before") {
    World w;
    CommandBuffer cmd;
    for (int i = 0; i < 100; ++i) {
        cmd.spawn(cbt::Pos{static_cast<float>(i)}, cbt::Vel{1.0f});
    }
    CHECK(w.entity_count() == 0); // deferred: nothing spawned yet
    CHECK(cmd.size() == 100);

    cmd.apply(w);
    CHECK(w.entity_count() == 100);
    CHECK(cmd.empty()); // buffer cleared after apply
    CHECK(w.query<cbt::Pos, cbt::Vel>().count() == 100);
}

TEST_CASE("recorded despawns take effect on apply") {
    World w;
    std::vector<Entity> es;
    es.reserve(200);
    for (int i = 0; i < 200; ++i) {
        es.push_back(w.spawn_with(cbt::Pos{static_cast<float>(i)}));
    }

    CommandBuffer cmd;
    for (std::size_t i = 0; i < es.size(); i += 2) {
        cmd.despawn(es[i]); // mark the evens
    }
    CHECK(w.entity_count() == 200); // deferred

    cmd.apply(w);
    CHECK(w.entity_count() == 100);
    bool ok = true;
    for (std::size_t i = 0; i < es.size(); ++i) {
        ok = ok && (w.is_alive(es[i]) == (i % 2 == 1)); // evens gone, odds alive
    }
    CHECK(ok);
}

TEST_CASE("recorded add/remove component take effect on apply") {
    World w;
    const Entity e = w.spawn_with(cbt::Pos{1.0f});

    CommandBuffer cmd;
    cmd.add_component(e, cbt::Added{5.0f});
    cmd.remove_component<cbt::Pos>(e);
    CHECK(w.has<cbt::Pos>(e)); // deferred: still as it was
    CHECK_FALSE(w.has<cbt::Added>(e));

    cmd.apply(w);
    CHECK_FALSE(w.has<cbt::Pos>(e));
    REQUIRE(w.has<cbt::Added>(e));
    CHECK(w.get<cbt::Added>(e)->a == 5.0f);
}

TEST_CASE("recording from inside par_for_each is thread-safe") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 20000;
    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(cbt::Pos{static_cast<float>(i)},
                                  cbt::Doomed{static_cast<std::int32_t>(i % 2)}));
    }

    CommandBuffer cmd;
    // Record a despawn for every odd entity — concurrently, from many workers, into one buffer.
    w.query<cbt::Pos, cbt::Doomed>().par_for_each(jobs, [&](Entity e, cbt::Pos&, cbt::Doomed& d) {
        if (d.v == 1) {
            cmd.despawn(e);
        }
    });
    CHECK(w.entity_count() == kN); // deferred
    CHECK(cmd.size() == kN / 2);

    cmd.apply(w);
    CHECK(w.entity_count() == kN / 2);
}

TEST_CASE("the schedule applies a system's commands at the phase boundary") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 8000;
    for (std::uint32_t i = 0; i < kN; ++i) {
        (void)w.spawn_with(cbt::Pos{static_cast<float>(i)},
                           cbt::Doomed{static_cast<std::int32_t>(i % 4 == 0 ? 1 : 0)});
    }

    Schedule s;
    // reaper reads Doomed and, for every 4th entity, records a deferred despawn. The schedule
    // applies its command buffer once the phase joins.
    s.add({"reaper",
           {signature_of<cbt::Doomed>(w), {}},
           [](World& world, JobSystem& j, CommandBuffer& cmd) {
               world.query<cbt::Doomed>().par_for_each(j, [&](Entity e, cbt::Doomed& d) {
                   if (d.v == 1) {
                       cmd.despawn(e);
                   }
               });
           }});

    CHECK(w.entity_count() == kN);
    s.run(w, jobs);
    CHECK(w.entity_count() == kN - kN / 4); // every 4th despawned after the phase
}
