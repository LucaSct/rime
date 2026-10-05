// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "terrain_fixture.hpp"

// m19.8c proof (f): every refusal is refused, and every refusal is COUNTED.
//
// CLAUDE.md guardrail 5: a skip, drop or defer path without a counter is a proof that reads as
// passing while it is blind. So each case here injects one failure, checks the module did the safe
// thing, and checks the specific counter for that reason moved — and no other refusal counter did.
using namespace rime;
using namespace rime_test;

namespace {

[[nodiscard]] FakeTileSource::Options small_world() {
    FakeTileSource::Options so;
    so.min_x = -4;
    so.max_x = 12;
    so.min_z = -4;
    so.max_z = 4;
    so.latency = 2;
    return so;
}

// One barrier with no simulation behind it: enough to drive requests and installs.
[[nodiscard]] tc::CommitStatus barrier(FakeTileSource& source,
                                       tc::TerrainCollision& terrain,
                                       std::uint64_t tick,
                                       std::span<const tc::Demand> demands = {}) {
    source.pump();
    const tc::Plan plan = terrain.plan(tick, demands);
    return terrain.try_commit(plan);
}

[[nodiscard]] std::uint64_t all_refusals(const tc::Counters& c) {
    return c.refused_stale_handle + c.refused_revision_mismatch + c.refused_invalid_content +
           c.refused_budget_overflow + c.refused_unknown_pin + c.refused_stale_plan +
           c.refused_required_failed;
}

// The activation schedule is determinism_test's subject. Here it is switched off, so that each
// barrier counted below is about the refusal being injected and not about a lead elapsing.
[[nodiscard]] tc::Config immediate() {
    tc::Config c = Sim::default_config();
    c.activation_lead_ticks = 0;
    return c;
}

const physics::Aabb kSpot = box_around({40.0f, 0.5f, 8.0f}, {0.5f, 0.5f, 0.5f}); // tile (2, 0)
const tc::TileKey kSpotTile{2, 0, 1};

} // namespace

TEST_CASE("m19.8c (f): a stale handle is refused and counted, never read, and the tile is asked "
          "for again") {
    FakeTileSource source(small_world());
    physics::PhysicsWorld world;
    tc::TerrainCollision terrain(immediate(), source, world);

    REQUIRE(terrain.request_admission(1, kSpot) == tc::AdmissionResult::Deferred);
    REQUIRE(barrier(source, terrain, 0) == tc::CommitStatus::Ready);
    REQUIRE(terrain.tile_state(kSpotTile) == tc::TileState::Requested);

    // Something else releases the slot out from under the module — the over-release ADR-0067 §4
    // says the asset server cannot always detect. The module's handle is now stale.
    REQUIRE(source.invalidate(kSpotTile));
    REQUIRE(source.live_slots() == 0);

    REQUIRE(barrier(source, terrain, 1) == tc::CommitStatus::Ready);
    CHECK(terrain.counters().refused_stale_handle == 1);
    CHECK(all_refusals(terrain.counters()) == 1);
    CHECK(terrain.counters().installs == 0);                          // nothing came out of it
    CHECK(terrain.tile_state(kSpotTile) == tc::TileState::Requested); // re-requested
    CHECK(terrain.counters().requests == 2);
    CHECK(terrain.admitted().empty());

    // The fresh request loads and installs normally, and the waiting entity is admitted on it.
    REQUIRE(barrier(source, terrain, 2) == tc::CommitStatus::Ready);
    REQUIRE(barrier(source, terrain, 3) == tc::CommitStatus::Ready);
    CHECK(terrain.tile_state(kSpotTile) == tc::TileState::Installed);
    REQUIRE(terrain.admitted().size() == 1);
    CHECK(terrain.admitted()[0] == 1);
    // The stale handle was never released (it was not ours to release any more); the fresh one
    // was, once, when its tile installed.
    CHECK(source.stale_releases == 0);
    CHECK(source.releases == 1);
    CHECK(source.live_slots() == 0);
}

TEST_CASE("m19.8c (f): a payload at the wrong revision is refused and counted; a body that needs "
          "it fails the commit rather than standing on it") {
    FakeTileSource source(small_world());
    source.set_payload_revision(kSpotTile.x, kSpotTile.z, 7); // the manifest says 1
    physics::PhysicsWorld world;
    tc::TerrainCollision terrain(immediate(), source, world);

    REQUIRE(terrain.request_admission(1, kSpot) == tc::AdmissionResult::Deferred);
    for (std::uint64_t tick = 0; tick < 6; ++tick) {
        REQUIRE(barrier(source, terrain, tick) == tc::CommitStatus::Ready);
    }
    CHECK(terrain.counters().refused_revision_mismatch == 1); // once — not once per barrier
    CHECK(all_refusals(terrain.counters()) == 1);
    CHECK(terrain.tile_state(kSpotTile) == tc::TileState::Failed);
    CHECK(terrain.counters().installs == 0);
    CHECK(world.body_count() == 0);
    CHECK(terrain.counters().requests == 1);   // no retry storm against a tile known to be bad
    CHECK(source.live_slots() == 0);           // the refused payload's handle was given back
    CHECK(terrain.deferred_admissions() == 1); // the entity is still held out…
    CHECK(terrain.counters().admission_retries >= 5); // …and that, too, is visible
    // It is not merely waiting, it is STUCK — its tile can never arrive — and that is counted
    // apart from an ordinary wait: once for the deferral, not once per barrier.
    CHECK(terrain.counters().admissions_blocked_by_failure == 1);

    // A simulated body over that tile cannot be waited into existence: explicit failure.
    const std::vector<tc::Demand> demands{{kSpot, tc::DemandKind::Body}};
    CHECK(barrier(source, terrain, 6, demands) == tc::CommitStatus::Failed);
    CHECK(terrain.counters().refused_required_failed == 1);
    CHECK(terrain.counters().stalls_admitted_body == 0); // Failed, not an endless Waiting
}

TEST_CASE("m19.8c (f): a pin at the wrong revision is refused; an unknown unpin is refused") {
    FakeTileSource source(small_world());
    physics::PhysicsWorld world;
    tc::TerrainCollision terrain(immediate(), source, world);

    // History recorded against revision 2 may not be replayed over revision-1 ground.
    const tc::PinToken wrong = terrain.pin(kSpot, 2, tc::PinReason::History);
    CHECK_FALSE(wrong.is_valid());
    CHECK(terrain.counters().refused_revision_mismatch == 1);
    CHECK(terrain.counters().pins_taken == 0);
    CHECK(terrain.tile_state(kSpotTile) == tc::TileState::Absent);

    const tc::PinToken right = terrain.pin(kSpot, 1, tc::PinReason::History);
    REQUIRE(right.is_valid());
    CHECK(terrain.pin_count(kSpotTile, tc::PinReason::History) == 1);
    CHECK(terrain.pin_count(kSpotTile, tc::PinReason::Query) == 0);
    CHECK(terrain.counters().pins_active == 1);

    CHECK(terrain.unpin(right));
    CHECK_FALSE(terrain.unpin(right)); // already released
    CHECK_FALSE(terrain.unpin(tc::PinToken{}));
    CHECK(terrain.counters().refused_unknown_pin == 2);
    CHECK(terrain.counters().pins_released == 1);
    CHECK(terrain.counters().pins_active == 0);
}

TEST_CASE("m19.8c (f): a required set over the cap fails explicitly, before anything is "
          "requested") {
    FakeTileSource source(small_world());
    physics::PhysicsWorld world;
    tc::Config config = immediate();
    config.max_required_tiles = 3;
    tc::TerrainCollision terrain(config, source, world);

    // A box over the corner where four tiles meet requires four.
    const std::vector<tc::Demand> corner{
        {box_around({16.0f, 0.5f, 16.0f}, {1.0f, 0.5f, 1.0f}), tc::DemandKind::Body}};
    source.pump();
    const tc::Plan plan = terrain.plan(0, corner);
    CHECK(plan.required.size() == 4);
    CHECK(plan.overflow);
    CHECK(terrain.try_commit(plan) == tc::CommitStatus::Failed);
    CHECK(terrain.try_commit(plan) == tc::CommitStatus::Failed);
    CHECK(terrain.counters().refused_budget_overflow == 1); // per plan, not per attempt
    CHECK(source.requests == 0);
    CHECK(world.body_count() == 0);

    // Three is within the cap: the same module carries on.
    const std::vector<tc::Demand> edge{
        {box_around({16.0f, 0.5f, 8.0f}, {1.0f, 0.5f, 1.0f}), tc::DemandKind::Body}};
    const tc::Plan ok = terrain.plan(1, edge);
    CHECK_FALSE(ok.overflow);
    CHECK(ok.required.size() == 2);

    // Pins count against the same cap: two required + two pinned elsewhere = four.
    REQUIRE(terrain.pin(box_around({100.0f, 0.5f, 16.0f}, {0.5f, 0.5f, 0.5f}), 1).is_valid());
    const tc::Plan with_pins = terrain.plan(2, edge);
    CHECK(with_pins.required_or_pinned == 4);
    CHECK(with_pins.overflow);

    // A box that is not a box — a NaN — is the same refusal, not a hang and not "no tiles".
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<tc::Demand> broken{
        {box_around({nan, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}), tc::DemandKind::Body}};
    CHECK(terrain.plan(3, broken).overflow);
    CHECK_FALSE(terrain.covers(broken[0].bounds));
    CHECK_FALSE(terrain.pin(broken[0].bounds, 1).is_valid());
}

TEST_CASE("m19.8c (f): a failed load and misplaced content are each refused under their own "
          "counter") {
    FakeTileSource source(small_world());
    source.set_fails(2, 0);
    source.set_misplaced(3, 0); // a tile whose cooked origin is 3 m off its grid cell
    physics::PhysicsWorld world;
    tc::TerrainCollision terrain(immediate(), source, world);

    REQUIRE(terrain.pin(box_around({56.0f, 0.5f, 8.0f}, {20.0f, 0.5f, 0.5f}), 1).is_valid());
    for (std::uint64_t tick = 0; tick < 5; ++tick) {
        REQUIRE(barrier(source, terrain, tick) == tc::CommitStatus::Ready);
    }
    CHECK(terrain.tile_state({2, 0, 1}) == tc::TileState::Failed);
    CHECK(terrain.tile_state({3, 0, 1}) == tc::TileState::Failed);
    CHECK(terrain.tile_state({4, 0, 1}) == tc::TileState::Installed);
    CHECK(terrain.counters().load_failures == 1);
    CHECK(terrain.counters().refused_invalid_content == 1);
    CHECK(all_refusals(terrain.counters()) == 1);
    CHECK(terrain.counters().installs == 1);
    CHECK(source.live_slots() == 0); // failed, refused and installed alike: all given back
    CHECK(source.releases == 3);
    CHECK_FALSE(terrain.require_coverage(box_around({56.0f, 0.5f, 8.0f}, {1.0f, 0.5f, 0.5f})));
    CHECK(terrain.counters().coverage_refusals == 1);
}

TEST_CASE("m19.8c (f): a superseded or already-committed plan is refused; a tile pinned after the "
          "plan is not evicted") {
    FakeTileSource source(small_world());
    physics::PhysicsWorld world;
    tc::TerrainCollision terrain(immediate(), source, world);

    // Install tile (2,0) under a pin, then drop the pin so the next plan lists it for eviction.
    const tc::PinToken first = terrain.pin(kSpot, 1);
    REQUIRE(first.is_valid());
    for (std::uint64_t tick = 0; tick < 4; ++tick) {
        REQUIRE(barrier(source, terrain, tick) == tc::CommitStatus::Ready);
    }
    REQUIRE(terrain.tile_state(kSpotTile) == tc::TileState::Installed);
    REQUIRE(terrain.unpin(first));

    const tc::Plan older = terrain.plan(4, {});
    REQUIRE(older.deactivate.size() == 1);
    const tc::Plan newer = terrain.plan(4, {});
    CHECK(terrain.try_commit(older) == tc::CommitStatus::Failed); // superseded
    CHECK(terrain.counters().refused_stale_plan == 1);
    CHECK(terrain.tile_state(kSpotTile) == tc::TileState::Installed); // …and it evicted nothing

    // Pinned between the plan and its commit: the planned eviction is skipped, and counted.
    const tc::PinToken late = terrain.pin(kSpot, 1);
    REQUIRE(late.is_valid());
    CHECK(terrain.try_commit(newer) == tc::CommitStatus::Ready);
    CHECK(terrain.counters().evictions_skipped_pinned == 1);
    CHECK(terrain.counters().evictions == 0);
    CHECK(terrain.tile_state(kSpotTile) == tc::TileState::Installed);

    CHECK(terrain.try_commit(newer) == tc::CommitStatus::Failed); // already committed
    CHECK(terrain.counters().refused_stale_plan == 2);

    // Unpinned for good: the next barrier evicts it, exactly once.
    REQUIRE(terrain.unpin(late));
    REQUIRE(barrier(source, terrain, 5) == tc::CommitStatus::Ready);
    CHECK(terrain.counters().evictions == 1);
    CHECK(terrain.tile_state(kSpotTile) == tc::TileState::Absent);
    CHECK(source.live_slots() == 0);
    CHECK(source.releases == 1);
    CHECK(world.body_count() == 0);
}
