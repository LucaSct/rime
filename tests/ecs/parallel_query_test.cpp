// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.4a (parallel system execution). Query<Ts...>::par_for_each runs a query body across
// all cores on the JobSystem, one chunk per task. We prove it touches exactly the matching set,
// exactly once, with the same result as the serial for_each — over many 16 KiB chunks, so the work
// genuinely spreads across workers. This is also the engine's first multicore load on the ECS
// storage, so it is a suite the Phase 0 ThreadSanitizer CI job now runs: a data race here fails CI.
//
// doctest macros stay on the main thread: par_for_each joins before returning, and the bodies touch
// only disjoint component rows and atomics, so every CHECK below runs after the join.

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/jobs.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"

using namespace rime::ecs;
using rime::core::JobSystem;

namespace pqt {
struct Pos {
    float x;
    float y;
};

struct Vel {
    float dx;
    float dy;
};

struct Tag {
    std::int32_t v;
};

struct NeverUsed {
    int z;
};
} // namespace pqt

TEST_CASE("par_for_each integrates every matching entity exactly once, across many chunks") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 20000; // {Pos, Vel} spans ~30 chunks -> real work per worker

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(pqt::Pos{static_cast<float>(i), 0.0f}, pqt::Vel{1.0f, 0.0f}));
    }
    // Non-matching entities (Tag only) that must never be visited.
    for (std::uint32_t i = 0; i < 128; ++i) {
        (void)w.spawn_with(pqt::Tag{static_cast<std::int32_t>(i)});
    }

    std::atomic<std::size_t> visited{0};
    w.query<pqt::Pos, pqt::Vel>().par_for_each(jobs, [&](pqt::Pos& p, pqt::Vel& v) {
        p.x += v.dx; // i -> i+1 exactly once (twice would give i+2, never would leave i)
        visited.fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(visited.load() == kN);
    bool all_once = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        all_once = all_once && (w.get<pqt::Pos>(es[i])->x == static_cast<float>(i) + 1.0f);
    }
    CHECK(all_once);
}

TEST_CASE("the Entity-first parallel body visits each matching entity exactly once") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 8000;

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(pqt::Pos{static_cast<float>(i), 0.0f}));
    }

    // One counter per entity slot. A fresh directory with no despawns hands out indices 0..kN-1, so
    // e.index is a dense key in [0, kN) here — safe to size the vector to kN.
    std::vector<std::atomic<int>> hits(kN);
    for (auto& h : hits) {
        h.store(0, std::memory_order_relaxed);
    }
    w.query<pqt::Pos>().par_for_each(jobs, [&](Entity e, pqt::Pos& p) {
        hits[e.index].fetch_add(1, std::memory_order_relaxed);
        p.y = p.x; // write-through proves the reference is live
    });

    bool each_once = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        each_once = each_once && (hits[es[i].index].load(std::memory_order_relaxed) == 1);
        each_once = each_once && (w.get<pqt::Pos>(es[i])->y == static_cast<float>(i));
    }
    CHECK(each_once);
}

TEST_CASE("par_for_each is deterministic: same result as the serial computation") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 12000;

    std::vector<Entity> es;
    es.reserve(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        es.push_back(w.spawn_with(pqt::Pos{static_cast<float>(i), 2.0f}, pqt::Vel{3.0f, 0.0f}));
    }

    // A per-row pure map: the order tasks run in cannot change any outcome, so parallel == serial.
    w.query<pqt::Pos, pqt::Vel>().par_for_each(
        jobs, [](pqt::Pos& p, pqt::Vel& v) { p.x = p.x * v.dx + p.y; });

    bool all_match = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        const float expected = static_cast<float>(i) * 3.0f + 2.0f;
        all_match = all_match && (w.get<pqt::Pos>(es[i])->x == expected);
    }
    CHECK(all_match);
}

TEST_CASE("par_for_each honors a coarser grain (several chunks per task)") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 10000;
    for (std::uint32_t i = 0; i < kN; ++i) {
        (void)w.spawn_with(pqt::Pos{static_cast<float>(i), 0.0f}, pqt::Vel{1.0f, 0.0f});
    }

    std::atomic<std::size_t> visited{0};
    w.query<pqt::Pos, pqt::Vel>().par_for_each(
        jobs,
        [&](pqt::Pos& p, pqt::Vel& v) {
            p.x += v.dx;
            visited.fetch_add(1, std::memory_order_relaxed);
        },
        8); // 8 chunks per task
    CHECK(visited.load() == kN);
}

TEST_CASE("par_for_each on an empty or unregistered query never calls the body") {
    World w;
    JobSystem jobs;
    (void)w.spawn_with(pqt::Pos{1.0f, 1.0f});

    std::atomic<int> calls{0};
    // NeverUsed is registered nowhere -> no entity can match -> no tasks, no calls.
    w.query<pqt::NeverUsed>().par_for_each(
        jobs, [&](pqt::NeverUsed&) { calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK(calls.load() == 0);

    // Registered but unmatched: a Pos-only and a Vel-only entity, none with BOTH.
    (void)w.spawn_with(pqt::Vel{0.0f, 0.0f});
    w.query<pqt::Pos, pqt::Vel>().par_for_each(
        jobs, [&](pqt::Pos&, pqt::Vel&) { calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK(calls.load() == 0);
}

TEST_CASE("the empty parallel query visits every entity across archetypes") {
    World w;
    JobSystem jobs;
    constexpr std::uint32_t kN = 6000;
    // Mix signatures so several archetypes (and all their chunk lists) get scanned.
    for (std::uint32_t i = 0; i < kN; ++i) {
        if (i % 3 == 0) {
            (void)w.spawn_with(pqt::Pos{0.0f, 0.0f});
        } else if (i % 3 == 1) {
            (void)w.spawn_with(pqt::Pos{0.0f, 0.0f}, pqt::Vel{0.0f, 0.0f});
        } else {
            (void)w.spawn();
        }
    }

    std::atomic<std::size_t> visited{0};
    w.query<>().par_for_each(jobs,
                             [&](Entity) { visited.fetch_add(1, std::memory_order_relaxed); });
    CHECK(visited.load() == kN);
    CHECK(w.entity_count() == kN);
}
