// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "terrain_fixture.hpp"

// m19.8c proofs (d) and (e): what stays, what goes, and how much.
//
//   (d) Seams. A body resting ACROSS a tile seam is on two tiles, and both stay — through any
//       amount of loading and unloading of the tiles further out.
//   (e) Hysteresis and plateau. Pacing over a boundary does not load and unload the same tile
//       every step, and a long journey holds a bounded amount of terrain however far it goes.
using namespace rime;
using namespace rime_test;

namespace {

[[nodiscard]] FakeTileSource::Options long_strip() {
    FakeTileSource::Options so;
    so.min_x = -4;
    so.max_x = 70;
    so.min_z = -1;
    so.max_z = 1;
    so.latency = 2;
    return so;
}

[[nodiscard]] int count_ops(const Sim& sim, const tc::TileKey& key, tc::JournalOp op) {
    int n = 0;
    for (const tc::JournalEntry& e : sim.terrain.journal()) {
        if (e.key == key && e.op == op) {
            ++n;
        }
    }
    return n;
}

// Is there a contact manifold between `body` and the static body of tile `key` right now?
[[nodiscard]] bool touching(const Sim& sim, physics::BodyId body, const tc::TileKey& key) {
    const physics::BodyId tile = sim.terrain.tile_body(key);
    if (!tile.is_valid()) {
        return false;
    }
    std::vector<physics::Manifold> manifolds;
    sim.world.compute_contacts(manifolds);
    for (const physics::Manifold& m : manifolds) {
        if ((m.a == body && m.b == tile) || (m.a == tile && m.b == body)) {
            return m.count > 0;
        }
    }
    return false;
}

} // namespace

TEST_CASE("m19.8c (d): a body resting across a seam keeps both tiles, and its contact survives a "
          "retain/evict cycle of the tiles beyond") {
    Sim sim(long_strip(), Sim::default_config());

    // A 2 m crate centred exactly on the x = 16 seam between tiles (0,0) and (1,0).
    const tc::TileKey left{0, 0, 1};
    const tc::TileKey right{1, 0, 1};
    Sim::Spawn crate;
    crate.entity = 1;
    crate.half = {1.0f, 0.5f, 1.0f};
    crate.desc.shape.type = physics::ShapeType::Box;
    crate.desc.shape.half_extents = crate.half;
    crate.desc.position = {16.0f, 0.55f, 8.0f};
    crate.desc.friction = 0.6f;
    REQUIRE(sim.spawn(crate) == tc::AdmissionResult::Deferred);
    // Admission needed BOTH tiles: a box on a seam is over two columns.
    CHECK(sim.terrain.pin_count(left, tc::PinReason::Admission) == 1);
    CHECK(sim.terrain.pin_count(right, tc::PinReason::Admission) == 1);

    // A runner that will go far away and come back, dragging the envelope with it.
    REQUIRE(sim.spawn_sphere(2, {20.0f, 0.5f, 8.0f}, {0.0f, 0.0f, 0.0f}, /*driven=*/true) ==
            tc::AdmissionResult::Deferred);
    REQUIRE(sim.run(120)); // admitted, dropped, settled
    REQUIRE(sim.actor(1) != nullptr);
    const physics::BodyId crate_body = sim.actor(1)->body;
    const core::Vec3 rest = sim.state_of(1).position;
    CHECK(std::fabs(rest.y - 0.5f) < 0.02f);
    REQUIRE(touching(sim, crate_body, left));
    REQUIRE(touching(sim, crate_body, right));

    // A tile just outside the crate's own two-tile retain envelope: tile 4 starts 31 m beyond the
    // crate's far edge. Only the runner ever keeps it resident.
    const tc::TileKey beyond{4, 0, 1};
    const physics::Aabb crate_box = box_around(rest, crate.half);

    float worst_drift = 0.0f;
    bool seam_always_held = true;
    bool contact_always_held = true;
    const auto leg = [&](float vx, int ticks) {
        sim.set_drive(2, {vx, 0.0f, 0.0f});
        for (int i = 0; i < ticks; ++i) {
            REQUIRE(sim.step() == tc::CommitStatus::Ready);
            seam_always_held = seam_always_held &&
                               sim.terrain.tile_state(left) == tc::TileState::Installed &&
                               sim.terrain.tile_state(right) == tc::TileState::Installed &&
                               sim.terrain.covers(crate_box);
            const core::Vec3 p = sim.state_of(1).position;
            worst_drift = std::max(worst_drift, core::length(p - rest));
            // Sampled, not every tick: compute_contacts walks the whole world.
            if (i % 16 == 0) {
                contact_always_held = contact_always_held && touching(sim, crate_body, left) &&
                                      touching(sim, crate_body, right);
            }
        }
    };
    leg(20.0f, 360);  // out to x ~ 140
    leg(-20.0f, 360); // and back
    leg(20.0f, 360);  // and out again

    // The far tile really was cycled: installed, evicted, installed again, evicted again.
    CHECK(count_ops(sim, beyond, tc::JournalOp::Activate) >= 2);
    CHECK(count_ops(sim, beyond, tc::JournalOp::Deactivate) >= 2);
    // The crate's own tiles were installed once and never touched.
    CHECK(count_ops(sim, left, tc::JournalOp::Activate) == 1);
    CHECK(count_ops(sim, right, tc::JournalOp::Activate) == 1);
    CHECK(count_ops(sim, left, tc::JournalOp::Deactivate) == 0);
    CHECK(count_ops(sim, right, tc::JournalOp::Deactivate) == 0);
    CHECK(seam_always_held);
    CHECK(contact_always_held);
    // It has almost certainly gone to SLEEP by now — and a sleeping body is still a demand. Its
    // position has not moved by a tenth of a millimetre through sixty-odd tile installs/removals.
    CHECK(worst_drift < 1e-4f);
    CHECK(sim.terrain.counters().stalls_admitted_body == 0);
}

namespace {

struct Pacing {
    std::uint64_t installs = 0;
    std::uint64_t evictions = 0;
    bool completed = false;
};

// Pace a body back and forth across the x = 16 seam, +/- 3 m, forty times; report what the pacing
// alone cost (the warm-up before it is excluded).
[[nodiscard]] Pacing pace(float retain_margin_tiles) {
    tc::Config config = Sim::default_config();
    config.retain_margin_tiles = retain_margin_tiles;
    Sim sim(long_strip(), config);
    Pacing out;
    (void)sim.spawn_sphere(1, {16.0f, 0.5f, 8.0f}, {0.0f, 0.0f, 0.0f}, /*driven=*/true);
    if (!sim.run(60)) {
        return out;
    }
    // One full cycle of warm-up so that the far end of the swing has been visited.
    const auto swing = [&](float vx) {
        sim.set_drive(1, {vx, 0.0f, 0.0f});
        return sim.run(36); // 36 ticks at 5 m/s = 3 m
    };
    if (!(swing(5.0f) && swing(-5.0f) && swing(-5.0f) && swing(5.0f))) {
        return out;
    }
    const tc::Counters before = sim.terrain.counters();
    for (int cycle = 0; cycle < 40; ++cycle) {
        if (!(swing(5.0f) && swing(-5.0f) && swing(-5.0f) && swing(5.0f))) {
            return out;
        }
    }
    out.installs = sim.terrain.counters().installs - before.installs;
    out.evictions = sim.terrain.counters().evictions - before.evictions;
    out.completed = sim.terrain.counters().stalls_admitted_body == 0;
    return out;
}

} // namespace

TEST_CASE("m19.8c (e): pacing across a boundary does not thrash — and without the hysteresis it "
          "does") {
    // Activate at one tile width, retain to two: the swing moves the activate edge back and forth
    // over a tile boundary, but never carries the RETAIN edge back over it.
    const Pacing with = pace(2.0f);
    REQUIRE(with.completed);
    CHECK(with.installs == 0);
    CHECK(with.evictions == 0);

    // The control: retain == activate. Now the same edge both loads and unloads, and forty cycles
    // of pacing cost dozens of tile installs for ground the body never left the neighbourhood of.
    // (This arm is what shows the zeroes above are the hysteresis and not a body that never came
    // near a boundary.)
    const Pacing without = pace(1.0f);
    REQUIRE(without.completed);
    CHECK(without.evictions >= 40);
    CHECK(without.installs >= 40);
}

TEST_CASE("m19.8c (e): resident terrain plateaus under a long traversal") {
    Sim sim(long_strip(), Sim::default_config());
    REQUIRE(sim.spawn_sphere(1, {8.0f, 0.5f, 8.0f}, {40.0f, 0.0f, 0.0f}, /*driven=*/true) ==
            tc::AdmissionResult::Deferred);

    // 1500 ticks at 40 m/s is a kilometre: sixty-odd tiles end to end, three rows deep.
    std::uint64_t peak_first_half = 0;
    std::uint64_t peak_second_half = 0;
    for (int i = 0; i < 1500; ++i) {
        REQUIRE(sim.step() == tc::CommitStatus::Ready);
        const std::uint64_t bytes = sim.terrain.counters().bytes_resident;
        CHECK(bytes == sim.terrain.counters().tiles_installed * kTileBytes);
        (i < 750 ? peak_first_half : peak_second_half) =
            std::max(i < 750 ? peak_first_half : peak_second_half, bytes);
    }
    const tc::Counters c = sim.terrain.counters();
    CHECK(sim.state_of(1).position.x > 900.0f);

    // A great deal of terrain went THROUGH the world…
    CHECK(c.installs > 150);
    CHECK(c.evictions > 130);
    // …while only a fixed window of it was ever IN the world: at most the retain envelope, which
    // is 2 tiles either side of the body's own (up to 2-tile-wide) footprint in x — six columns —
    // by the strip's three rows. The peak is the envelope, not the distance travelled.
    CHECK(c.peak_bytes_resident <= 18 * kTileBytes);
    CHECK(peak_second_half == peak_first_half); // flat, not creeping
    CHECK(c.installs - c.evictions == c.tiles_installed);
    // The asset side: an INSTALLED tile holds no ownership at all — the handle goes back the moment
    // physics has its copy — so what is live in the source is only what is still loading, and
    // every request was released exactly once: at install, or at cancellation.
    CHECK(sim.source.releases == c.installs + c.requests_cancelled);
    CHECK(sim.source.requests == sim.source.releases + sim.source.live_slots());
    CHECK(sim.source.peak_live <= 9); // in flight: the leading edge, a few columns deep at most
    CHECK(sim.source.stale_releases == 0);
    CHECK(c.refused_stale_handle == 0);
    CHECK(c.stalls_admitted_body == 0);
}

TEST_CASE("m19.8c: destroying the module returns every body, shape and handle") {
    FakeTileSource source(long_strip());
    physics::PhysicsWorld world;
    {
        tc::TerrainCollision terrain(Sim::default_config(), source, world);
        const tc::PinToken token =
            terrain.pin(box_around({40.0f, 0.0f, 8.0f}, {20.0f, 1.0f, 1.0f}), 1);
        REQUIRE(token.is_valid());
        std::uint64_t tick = 0;
        const auto barrier = [&] {
            source.pump();
            const tc::Plan plan = terrain.plan(tick++, {});
            REQUIRE(terrain.try_commit(plan) == tc::CommitStatus::Ready);
        };
        barrier();
        CHECK(source.live_slots() == 3); // requested: three ownerships held while loading
        for (int i = 0; i < 9; ++i) {
            barrier();
        }
        CHECK(terrain.counters().tiles_installed == 3);
        CHECK(world.body_count() == 3);
        CHECK(source.live_slots() == 0); // installed: every handle already given back
        CHECK(source.releases == 3);

        // One more tile, left mid-load: the destructor has a handle to return as well as bodies.
        REQUIRE(terrain.pin(box_around({200.0f, 0.0f, 8.0f}, {1.0f, 1.0f, 1.0f}), 1).is_valid());
        barrier();
        CHECK(source.live_slots() == 1);
    }
    CHECK(source.releases == 4);
    CHECK(world.body_count() == 0);
    CHECK(source.live_slots() == 0);
    CHECK(source.stale_releases == 0);
}
