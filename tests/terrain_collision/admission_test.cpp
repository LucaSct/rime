// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "terrain_fixture.hpp"

// m19.8c proofs (b) and (c): the two things that can happen when ground is missing.
//
//   (b) A body ARRIVING — a spawn, a teleport — is held out. The world keeps ticking without it,
//       and it enters on the first barrier at which ALL of its tiles are installed.
//   (c) A body ALREADY SIMULATED that outruns the prefetch envelope cannot be held out, so the
//       tick waits. That is the safety net; it is counted, and with a correctly sized envelope the
//       count is zero.
//
// In neither case does anything simulate over ground that is not there.
using namespace rime;
using namespace rime_test;

namespace {

[[nodiscard]] FakeTileSource::Options flat_strip() {
    FakeTileSource::Options so;
    so.min_x = -4;
    so.max_x = 20;
    so.min_z = -3;
    so.max_z = 3;
    so.latency = 2;
    return so;
}

// The tick an Activate for `key` was journalled on, or -1.
[[nodiscard]] std::int64_t activation_tick(const Sim& sim, const tc::TileKey& key) {
    for (const tc::JournalEntry& e : sim.terrain.journal()) {
        if (e.key == key && e.op == tc::JournalOp::Activate) {
            return static_cast<std::int64_t>(e.tick);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("m19.8c (b): a teleport into uninstalled terrain is held while the world advances, and "
          "admitted on the first barrier after ALL its tiles install") {
    FakeTileSource::Options so = flat_strip();
    // The destination straddles the z = 0 seam at x = 168: tiles (10,-1) and (10,0). They load at
    // different speeds, so "one of them is in" and "both are in" are different barriers — which is
    // what lets this case tell "all" from "any".
    const tc::TileKey fast{10, 0, 1};
    const tc::TileKey slow{10, -1, 1};
    Sim sim(so, Sim::default_config());
    // `fast` loads inside the default lead of 8 and is installed on schedule, at +8; `slow` takes
    // 20 pumps, misses its scheduled tick, and is installed late — at +20.
    sim.source.set_latency(fast.x, fast.z, 4);
    sim.source.set_latency(slow.x, slow.z, 20);

    // Two bodies near the origin: the witness that keeps moving, and the one that will teleport.
    REQUIRE(sim.spawn_sphere(1, {4.0f, 0.6f, 4.0f}, {3.0f, 0.0f, 0.0f}, /*driven=*/true) ==
            tc::AdmissionResult::Deferred);
    REQUIRE(sim.spawn_sphere(2, {8.0f, 0.6f, 8.0f}) == tc::AdmissionResult::Deferred);
    REQUIRE(sim.run(20));
    REQUIRE(sim.actor(1) != nullptr);
    REQUIRE(sim.actor(2) != nullptr);

    const core::Vec3 destination{168.0f, 0.6f, 0.0f};
    const physics::Aabb landing = box_around(destination, {0.5f, 0.5f, 0.5f});
    const std::uint64_t deferred_before = sim.terrain.counters().admissions_deferred;
    const std::uint64_t teleport_tick = sim.tick;

    REQUIRE(sim.teleport(2, destination) == tc::AdmissionResult::Deferred);
    CHECK(sim.terrain.counters().admissions_deferred == deferred_before + 1);
    CHECK(sim.terrain.deferred_admissions() == 1);
    CHECK(sim.actor(2) == nullptr); // held OUT of the simulation, not parked inside it
    // Its tiles are pinned for the arrival, and not yet requested: requests are a barrier's job.
    CHECK(sim.terrain.pin_count(fast, tc::PinReason::Admission) == 1);
    CHECK(sim.terrain.pin_count(slow, tc::PinReason::Admission) == 1);
    CHECK(sim.terrain.tile_state(slow) == tc::TileState::Pending);

    // Tick until it is admitted, watching the hold.
    int held_ticks = 0;
    int held_with_one_tile_in = 0;
    float witness_x = sim.state_of(1).position.x;
    while (sim.actor(2) == nullptr) {
        REQUIRE(held_ticks < 200);
        REQUIRE(sim.step() == tc::CommitStatus::Ready);
        if (sim.actor(2) != nullptr) {
            break;
        }
        ++held_ticks;
        // The rest of the world is not waiting for it: the witness moved THIS tick…
        const float x = sim.state_of(1).position.x;
        CHECK(x > witness_x);
        witness_x = x;
        // …and while it is held, its landing box is by definition not covered.
        CHECK_FALSE(sim.terrain.covers(landing));
        if (sim.terrain.tile_state(fast) == tc::TileState::Installed) {
            ++held_with_one_tile_in;
        }
    }

    // It was held across several ticks, including ticks on which ONE of its two tiles was already
    // installed — the window in which admitting on "any tile" would have let it in.
    CHECK(held_ticks >= 10);
    CHECK(held_with_one_tile_in >= 5);

    // Admitted exactly on the barrier that installed its LAST tile — never earlier, and not later.
    const std::int64_t fast_tick = activation_tick(sim, fast);
    const std::int64_t slow_tick = activation_tick(sim, slow);
    REQUIRE(fast_tick >= 0);
    REQUIRE(slow_tick > fast_tick);
    REQUIRE(sim.admission_ticks.size() == 3); // the two spawns, then this
    CHECK(sim.admission_ticks.back().first == 2);
    CHECK(static_cast<std::int64_t>(sim.admission_ticks.back().second) == slow_tick);
    CHECK(sim.admission_ticks.back().second > teleport_tick);
    CHECK(sim.terrain.counters().admitted_after_deferral == 3);
    CHECK(sim.terrain.counters().admission_retries > 0);
    CHECK(sim.terrain.deferred_admissions() == 0);

    // On schedule and late, respectively — and the late one is counted, once.
    CHECK(fast_tick == static_cast<std::int64_t>(teleport_tick) + 8);
    CHECK(slow_tick == static_cast<std::int64_t>(teleport_tick) + 20);
    CHECK(sim.terrain.counters().install_late == 1);

    // The whole episode was deferral, not the stall.
    CHECK(sim.terrain.counters().stalls_admitted_body == 0);
    CHECK(sim.waiting_pumps == 0);
    CHECK(sim.tick == sim.hashes.size()); // every tick was simulated; none was skipped or repeated

    // And it landed on ground: one barrier later the admission pin has been handed over to the
    // body's own demand, the tiles are still there, and the body comes to rest on them.
    REQUIRE(sim.run(120));
    CHECK(sim.terrain.pin_count(fast, tc::PinReason::Admission) == 0);
    CHECK(sim.terrain.pin_count(slow, tc::PinReason::Admission) == 0);
    CHECK(sim.terrain.tile_state(fast) == tc::TileState::Installed);
    CHECK(sim.terrain.tile_state(slow) == tc::TileState::Installed);
    const physics::BodyState landed = sim.state_of(2);
    CHECK(landed.position.y > 0.45f);
    CHECK(landed.position.y < 0.6f);
}

TEST_CASE("m19.8c (b): an admission whose ground is already installed is immediate, and a "
          "cancelled one lets its tiles go") {
    Sim sim(flat_strip(), Sim::default_config());
    REQUIRE(sim.spawn_sphere(1, {4.0f, 0.6f, 4.0f}) == tc::AdmissionResult::Deferred);
    REQUIRE(sim.run(10));

    // Right beside body 1: the ground is there, so there is nothing to wait for.
    CHECK(sim.spawn_sphere(2, {6.0f, 0.6f, 6.0f}) == tc::AdmissionResult::Admitted);
    CHECK(sim.terrain.counters().admissions_immediate == 1);
    CHECK(sim.actor(2) != nullptr);

    // Far away, then cancelled before it could arrive.
    const tc::TileKey far{15, 2, 1};
    const physics::Aabb there = box_around({248.0f, 0.6f, 40.0f}, {0.5f, 0.5f, 0.5f});
    REQUIRE(sim.terrain.request_admission(9, there) == tc::AdmissionResult::Deferred);
    REQUIRE(sim.step() == tc::CommitStatus::Ready);
    CHECK(sim.terrain.tile_state(far) == tc::TileState::Requested);
    CHECK(sim.terrain.cancel_admission(9));
    CHECK_FALSE(sim.terrain.cancel_admission(9));
    CHECK(sim.terrain.counters().admissions_cancelled == 1);
    REQUIRE(sim.step() == tc::CommitStatus::Ready);
    // Unpinned and under nobody: the request is given back, uninstalled, and counted as such.
    CHECK(sim.terrain.tile_state(far) == tc::TileState::Absent);
    CHECK(sim.terrain.counters().requests_cancelled == 1);
    CHECK(sim.terrain.counters().pins_active == 0);
}

namespace {

struct Walk {
    std::uint64_t stalled_ticks = 0;
    std::uint64_t stall_retries = 0;
    std::uint64_t waiting_pumps = 0;
    std::uint64_t install_late = 0;
    std::uint64_t installs_required_early = 0;
    float lowest_y = 1.0e9f;
    float final_x = 0.0f;
    std::uint64_t ticks = 0;
    std::size_t hashes = 0;
    bool completed = false;
};

// A body driven at a steady 5 m/s from tile 0 toward tile 1 and beyond, with a slow loader.
[[nodiscard]] Walk walk_into_unloaded(float activate_margin_tiles) {
    FakeTileSource::Options so = flat_strip();
    so.latency = 40; // slow disk: forty pumps per tile

    tc::Config config = Sim::default_config();
    config.activate_margin_tiles = activate_margin_tiles;
    config.retain_margin_tiles = std::max(activate_margin_tiles, 2.0f);

    Sim sim(so, config);
    Walk out;
    (void)sim.spawn_sphere(1, {8.0f, 0.5f, 8.0f}, {5.0f, 0.0f, 0.0f}, /*driven=*/true);
    for (int i = 0; i < 420; ++i) {
        if (sim.step() != tc::CommitStatus::Ready) {
            return out;
        }
        if (sim.actor(1) != nullptr) {
            out.lowest_y = std::min(out.lowest_y, sim.state_of(1).position.y);
        }
    }
    out.stalled_ticks = sim.terrain.counters().stalls_admitted_body;
    out.stall_retries = sim.terrain.counters().stall_retries;
    out.waiting_pumps = sim.waiting_pumps;
    out.install_late = sim.terrain.counters().install_late;
    out.installs_required_early = sim.terrain.counters().installs_required_early;
    out.final_x = sim.state_of(1).position.x;
    out.ticks = sim.tick;
    out.hashes = sim.hashes.size();
    out.completed = true;
    return out;
}

} // namespace

TEST_CASE("m19.8c (c): outrunning a too-small prefetch envelope stalls the tick (counted) instead "
          "of dropping the body through; a correct envelope never stalls") {
    // Envelope of ZERO tile widths: a tile is not even requested until the body's swept box is
    // already on it. With forty pumps of load time, the ground cannot possibly be there in time.
    const Walk starved = walk_into_unloaded(0.0f);
    REQUIRE(starved.completed);
    // The safety net fired, and it is visible: one stalled tick per tile boundary crossed, each
    // costing about forty retries.
    CHECK(starved.stalled_ticks >= 1);
    CHECK(starved.stall_retries >= 30);
    CHECK(starved.waiting_pumps == starved.stall_retries);
    // The body got where it was going (so it really did cross onto the late tile)…
    CHECK(starved.final_x > 24.0f);
    // …and at no tick was it below the ground. A 0.5 m sphere resting at y = 0 sits at 0.5; a body
    // that had been stepped over a missing tile for forty ticks would have fallen metres.
    CHECK(starved.lowest_y > 0.4f);
    // A stall holds the tick: every tick was simulated exactly once.
    CHECK(starved.ticks == 420);
    CHECK(starved.hashes == 420);
    // Each of those tiles was required the tick it was requested — before its scheduled tick — so
    // it was installed early, on the tick the stall held, and counted as such.
    CHECK(starved.installs_required_early >= 2);

    // The same walk with the default one-tile envelope: the tile is requested 16 m — over three
    // seconds — ahead of a body covering 0.083 m per tick. It is installed long before it is
    // needed.
    const Walk fed = walk_into_unloaded(1.0f);
    REQUIRE(fed.completed);
    CHECK(fed.stalled_ticks == 0);
    CHECK(fed.stall_retries == 0);
    CHECK(fed.waiting_pumps == 0);
    CHECK(fed.final_x > 24.0f);
    CHECK(fed.lowest_y > 0.4f);
    // Forty pumps is five times the default lead: every one of these loads missed its scheduled
    // tick, and the counter says so. Late is not the same as missing — the envelope is 16 m deep,
    // the tiles were in long before the body arrived, and the ground was never empty.
    CHECK(fed.install_late > 0);
    CHECK(fed.installs_required_early == 0);
}

TEST_CASE("m19.8c (c): a sweep over unloaded terrain stalls too, under its own counter") {
    Sim sim(flat_strip(), Sim::default_config());
    REQUIRE(sim.spawn_sphere(1, {4.0f, 0.6f, 4.0f}) == tc::AdmissionResult::Deferred);
    REQUIRE(sim.run(10));
    CHECK(sim.terrain.counters().stalls_sweep == 0);

    // A hitscan out to x = 150: nothing is loaded there, and the shot is evaluated THIS tick.
    physics::Aabb shot;
    shot.min = {140.0f, 0.0f, 3.5f};
    shot.max = {150.0f, 2.0f, 4.5f};
    sim.sweeps.push_back(shot);
    REQUIRE(sim.step() == tc::CommitStatus::Ready);
    sim.sweeps.clear();

    CHECK(sim.terrain.counters().stalls_sweep == 1);
    CHECK(sim.terrain.counters().stalls_admitted_body == 0); // not the body's envelope's fault
    CHECK(sim.waiting_pumps > 0);
    // By the time the tick ran, the ground under the shot was there.
    CHECK(sim.terrain.tile_state({8, 0, 1}) == tc::TileState::Installed);
    CHECK(sim.terrain.tile_state({9, 0, 1}) == tc::TileState::Installed);
}
