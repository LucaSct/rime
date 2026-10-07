// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>

#include "rime/assets/asset_server.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/physics/world.hpp"
#include "rime/terrain_collision/asset_server_source.hpp"
#include "rime/terrain_collision/terrain_collision.hpp"

// m19.8c end to end: the module over the REAL AssetServer (m19.8b's streamed handles) and a real
// cooked heightfield, rather than over the fixture's fake. The other proofs establish the barrier's
// behaviour; this one establishes that the production source honours the contract they assume —
// one request per record, one release per record, nothing resident afterwards.
//
// The only "wait" is AssetServer::wait_for_pending_loads(), a join on the job counter (the
// m19.8b tests' own synchronisation) — not a sleep.
using namespace rime;
namespace tc = rime::terrain_collision;

namespace {

const std::filesystem::path kTerrain =
    std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / "terrain.rhf"; // 5x4 samples

} // namespace

TEST_CASE("m19.8c: tiles stream through the AssetServer — requested, installed, evicted, and the "
          "server ends with nothing resident") {
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);

    // Learn the fixture's own geometry rather than restating it here: the grid is configured so
    // that this one cooked tile is exactly one grid cell at its own origin.
    const assets::HeightfieldAssetHandle probe = server.request_heightfield(kTerrain);
    server.wait_for_pending_loads();
    (void)server.pump();
    REQUIRE(server.state(probe) == assets::AssetState::Ready);
    const assets::HeightfieldAsset* cooked = server.get(probe);
    REQUIRE(cooked != nullptr);
    tc::Config config;
    config.tile_size_x = static_cast<float>(cooked->columns - 1) * cooked->cell_size_x;
    config.tile_size_z = static_cast<float>(cooked->rows - 1) * cooked->cell_size_z;
    config.origin = {cooked->origin.x, 0.0f, cooked->origin.z};
    const core::Vec3 centre{cooked->origin.x + 0.5f * config.tile_size_x,
                            cooked->origin.y,
                            cooked->origin.z + 0.5f * config.tile_size_z};
    // `cooked` dangles after the release below (ADR-0067: a streamed pointer lives only as long as
    // the ownership), so take what is still needed out of it first.
    const std::uint64_t cooked_bytes =
        std::uint64_t{cooked->columns} * cooked->rows * sizeof(std::uint16_t);
    REQUIRE(server.release(probe));
    REQUIRE(server.live_heightfield_slots() == 0);
    const assets::StreamCounters before = server.stream_counters();

    tc::AssetServerTileSource source(server);
    source.set_tile({0, 0, 3, kTerrain});
    physics::PhysicsWorld world;
    {
        tc::TerrainCollision terrain(config, source, world);
        physics::Aabb spot;
        spot.min = centre - core::Vec3{0.1f, 1.0f, 0.1f};
        spot.max = centre + core::Vec3{0.1f, 1.0f, 0.1f};

        // The manifest is at revision 3: a pin against revision 2 is refused.
        CHECK_FALSE(terrain.pin(spot, 2).is_valid());
        CHECK(terrain.counters().refused_revision_mismatch == 1);

        REQUIRE(terrain.request_admission(1, spot) == tc::AdmissionResult::Deferred);
        const tc::TileKey key{0, 0, 3};

        // Barrier 0 requests. The job may well have finished by the time we look — it does not
        // matter: without a pump() the server still says Loading, and without a barrier nothing
        // installs either way.
        const tc::Plan first = terrain.plan(0, {});
        REQUIRE(terrain.try_commit(first) == tc::CommitStatus::Ready);
        CHECK(terrain.tile_state(key) == tc::TileState::Requested);
        CHECK(source.outstanding() == 1);
        server.wait_for_pending_loads();
        CHECK(terrain.tile_state(key) == tc::TileState::Requested); // completion populated storage
        CHECK(world.body_count() == 0);                             // …and installed nothing
        (void)server.pump();
        CHECK(world.body_count() == 0); // still nothing: readiness is read only at the barrier

        // Ready, and STILL not installed: the tile has a scheduled tick (request + the default
        // lead), and being loaded early does not bring it forward.
        const std::uint64_t lead = terrain.config().activation_lead_ticks;
        REQUIRE(lead > 1);
        for (std::uint64_t tick = 1; tick < lead; ++tick) {
            const tc::Plan waiting = terrain.plan(tick, {});
            REQUIRE(terrain.try_commit(waiting) == tc::CommitStatus::Ready);
            CHECK(terrain.tile_state(key) == tc::TileState::Requested);
            CHECK(world.body_count() == 0);
        }
        CHECK(terrain.counters().installs_held_for_lead == lead - 1);

        const tc::Plan second = terrain.plan(lead, {});
        REQUIRE(terrain.try_commit(second) == tc::CommitStatus::Ready);
        CHECK(terrain.tile_state(key) == tc::TileState::Installed);
        CHECK(world.body_count() == 1);
        CHECK(terrain.counters().install_late == 0);
        REQUIRE(terrain.admitted().size() == 1);
        // The handle went back to the server the moment physics had its copy: the tile is
        // installed and the asset server holds nothing for it.
        CHECK(source.outstanding() == 0);
        CHECK(server.live_heightfield_slots() == 0);
        CHECK(terrain.counters().bytes_resident == cooked_bytes);

        // The installed tile is real ground: a ray from above hits it.
        physics::Ray ray;
        ray.origin = {centre.x, centre.y + 500.0f, centre.z};
        ray.direction = {0.0f, -1.0f, 0.0f};
        ray.max_distance = 2000.0f;
        physics::RayHit hit;
        CHECK(world.raycast(ray, hit));

        // Nobody became a demand, so at the next barrier (the admission pin is dropped by its plan)
        // the tile is gone.
        const tc::Plan third = terrain.plan(lead + 1, {});
        REQUIRE(terrain.try_commit(third) == tc::CommitStatus::Ready);
        CHECK(terrain.tile_state(key) == tc::TileState::Absent);
        CHECK(world.body_count() == 0);
        CHECK(source.outstanding() == 0);
        CHECK(terrain.counters().evictions == 1);
        CHECK_FALSE(world.raycast(ray, hit));

        // Ask again, and tear the module down mid-load: the destructor gives the handle back.
        REQUIRE(terrain.request_admission(2, spot) == tc::AdmissionResult::Deferred);
        const tc::Plan fourth = terrain.plan(lead + 2, {});
        REQUIRE(terrain.try_commit(fourth) == tc::CommitStatus::Ready);
        CHECK(source.outstanding() == 1);
    }
    server.wait_for_pending_loads();
    (void)server.pump();

    CHECK(source.outstanding() == 0);
    CHECK(server.live_heightfield_slots() == 0);
    CHECK(server.resident_heightfields() == 0);
    const assets::StreamCounters after = server.stream_counters();
    CHECK(after.loads_started - before.loads_started == 2);
    CHECK(after.evictions - before.evictions == 2);
    CHECK(after.stale_handle_resolutions == before.stale_handle_resolutions);
    CHECK(after.failed_loads == before.failed_loads);
}
