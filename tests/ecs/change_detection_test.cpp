// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M7.6 proofs for CHANGE DETECTION (ADR-0018 §4, finally made real). A monotonic world version, a
// per-column write stamp on every chunk, and a Query::for_each_changed that skips chunks nothing
// touched since a caller's checkpoint — the mechanism the editor's live sync (M9), physics
// write-back (this brick), and replication deltas (M11) all ride. All pure CPU, driving the public
// World/Query/Schedule seam.

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/schedule.hpp"
#include "rime/ecs/system.hpp"
#include "rime/ecs/world.hpp"

using namespace rime;

namespace {
struct Position {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Velocity {
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
};
} // namespace

TEST_CASE("M7.6: the world version starts at 1 and advances monotonically") {
    ecs::World w;
    CHECK(w.version() == 1);
    CHECK(w.advance_version() == 2);
    CHECK(w.version() == 2);
    CHECK(w.advance_version() == 3);
}

TEST_CASE("M7.6: adding a component stamps its column changed") {
    ecs::World w;
    const ecs::Entity e = w.spawn_with(Position{1.0f, 2.0f, 3.0f});

    // changed_since(0) matches everything ever written — the fresh Position qualifies.
    int seen = 0;
    w.query<Position>().for_each_changed(0, [&](Position&) { ++seen; });
    CHECK(seen == 1);

    // Nothing has changed *after* the current version, so a checkpoint at "now" sees nothing.
    seen = 0;
    w.query<Position>().for_each_changed(w.version(), [&](Position&) { ++seen; });
    CHECK(seen == 0);
    (void)e;
}

TEST_CASE("M7.6: mark_changed after advancing reports a get<>()-mutated component") {
    ecs::World w;
    const ecs::Entity e = w.spawn_with(Position{0.0f, 0.0f, 0.0f});
    const ecs::Version checkpoint = w.version(); // 1 — the spawn's stamp is at exactly this version

    w.advance_version(); // a new tick
    w.get<Position>(e)->x = 9.0f;
    w.mark_changed<Position>(e); // the writer discipline: report the get<>() write

    int seen = 0;
    w.query<Position>().for_each_changed(checkpoint, [&](Position& p) {
        ++seen;
        CHECK(p.x == doctest::Approx(9.0f));
    });
    CHECK(seen == 1); // only the post-checkpoint write shows up, not the spawn at `checkpoint`
}

TEST_CASE("M7.6: for_each_changed skips chunks that did not change") {
    ecs::World w;
    // Two archetypes, both carrying Position: {Position} and {Position, Velocity}.
    const ecs::Entity a = w.spawn_with(Position{1.0f, 0.0f, 0.0f});
    const ecs::Entity b = w.spawn_with(Position{2.0f, 0.0f, 0.0f}, Velocity{});

    const ecs::Version checkpoint = w.version();
    w.advance_version();

    // Touch only `a`.
    w.get<Position>(a)->x = 9.0f;
    w.mark_changed<Position>(a);

    std::vector<float> seen;
    w.query<Position>().for_each_changed(checkpoint, [&](Position& p) { seen.push_back(p.x); });

    // `b`'s chunk was untouched since the checkpoint, so it is skipped entirely — only `a` is seen.
    CHECK(seen.size() == 1);
    CHECK(seen[0] == doctest::Approx(9.0f));
    (void)b;
}

TEST_CASE("M7.6: a structural move stamps the relocated columns changed") {
    ecs::World w;
    const ecs::Entity e = w.spawn_with(Position{1.0f, 2.0f, 3.0f});
    const ecs::Version checkpoint = w.version();
    w.advance_version();

    // Adding Velocity relocates e to {Position, Velocity}; its Position rides along into a fresh
    // chunk, and both columns are "newly written there" — a consumer replicating structural moves
    // must see both.
    w.add_component<Velocity>(e, Velocity{});

    int pos = 0;
    int vel = 0;
    w.query<Position>().for_each_changed(checkpoint, [&](Position&) { ++pos; });
    w.query<Velocity>().for_each_changed(checkpoint, [&](Velocity&) { ++vel; });
    CHECK(pos == 1);
    CHECK(vel == 1);
}

TEST_CASE("M7.6: Schedule::run advances the world version once per run") {
    ecs::World w;
    core::JobSystem jobs(0);
    ecs::Schedule schedule; // no systems — a run still ticks the version

    const ecs::Version before = w.version();
    schedule.run(w, jobs);
    CHECK(w.version() == before + 1);
    schedule.run(w, jobs);
    CHECK(w.version() == before + 2);
}

TEST_CASE("M7.6: par_for_each_changed visits the same entities as the serial form") {
    ecs::World w;
    core::JobSystem jobs(0);

    // Enough entities to span several chunks (16 KiB / a ~28-byte row ⇒ a few hundred per chunk).
    constexpr int kCount = 4000;
    std::vector<ecs::Entity> entities;
    entities.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        entities.push_back(w.spawn_with(Position{static_cast<float>(i), 0.0f, 0.0f}, Velocity{}));
    }

    const ecs::Version checkpoint = w.version();
    w.advance_version();

    // Bump every entity's Velocity and report it — this stamps Velocity on the touched chunks.
    w.query<Velocity>().for_each([&](ecs::Entity e, Velocity& v) {
        v.dx = 1.0f;
        w.mark_changed<Velocity>(e);
    });

    std::atomic<int> par_count{0};
    w.query<Velocity>().par_for_each_changed(
        jobs, checkpoint, [&](Velocity&) { par_count.fetch_add(1); });

    int serial_count = 0;
    w.query<Velocity>().for_each_changed(checkpoint, [&](Velocity&) { ++serial_count; });

    CHECK(serial_count == kCount);
    CHECK(par_count.load() == kCount);
}
