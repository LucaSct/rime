// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the AssetServer's STREAMED kinds (m19.8b, ADR-0067): heightfields and terrain layers
// are requested like every other asset, but each request is an ownership that can be released, and
// the last release evicts. The properties, none checked with a sleep:
//
//   a. EVICTION FREES. request → Ready → release leaves no occupied slot and no resident payload,
//      and the eviction counter moves by exactly one.
//   b. GENERATIONS. After an evicted index is reused by a different file, the old handle resolves
//      to "not available" — never to the new tenant — and cannot release it either.
//   c. DEFERRED EVICTION. A release that lands while the load job is in flight frees nothing; the
//      slot stays pinned (its index is not reused) until the job finishes, and then it is evicted
//      exactly once.
//   d. COALESCING WITH OWNERSHIP. Two requests for one path are one physical load and two
//      ownerships; the asset stays resident until the second release.
//   e. Everything runs on the Rust-cooked fixtures (terrain.rhf, terrain_splat.rhf,
//      terrain_layer.rtl), so the async path is checked against the same bytes as the reader.
//
// HOW "IN FLIGHT" IS MADE DETERMINISTIC. The job system has no pause button, so the tests that
// need a load to be provably unfinished build a one-worker JobSystem and park that worker inside
// a job that waits on a gate (WorkerGate). The submitting thread only runs jobs inside wait(), so
// between "the worker is parked" and "the gate opens" a submitted load job sits in the queue with
// nobody able to run it: in flight for exactly as long as the test says, with no timing involved.
//
// Falsified (m19.8b commit body): dropping the generation comparison turns (b) red; evicting in
// release() regardless of in_flight turns (c) red — as a counted logic failure, not a crash.

#include <doctest/doctest.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/platform/filesystem.hpp"

namespace fs = std::filesystem;
using namespace rime::assets;
using rime::core::JobSystem;

namespace {

const fs::path kFixtures{RIME_ASSETS_FIXTURE_DIR};
const fs::path kTerrain = kFixtures / "terrain.rhf";            // 5x4 samples, no splat block
const fs::path kTerrainSplat = kFixtures / "terrain_splat.rhf"; // 5x4 samples + a 3x2 splat map
const fs::path kLayer = kFixtures / "terrain_layer.rtl";

struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// The blocking decode — the yardstick the streamed path must reproduce.
HeightfieldAsset sync_read_heightfield(const fs::path& p) {
    const auto bytes = rime::platform::read_file(p);
    REQUIRE(bytes);
    AssetError err{};
    auto hf = read_heightfield(*bytes, err);
    REQUIRE_MESSAGE(hf, "sync read_heightfield failed: ", to_string(err));
    return *hf;
}

// Drain every in-flight load and promote the results: the "load these now" idiom.
void settle(AssetServer& server) {
    server.wait_for_pending_loads();
    server.pump();
}

// Parks the single worker of a one-worker JobSystem inside a job until open() — see the file
// comment. The constructor returns only once the worker is provably inside the parked job; the
// destructor opens the gate and joins, so a failed REQUIRE mid-test cannot leave the worker
// blocked forever under the JobSystem's own destructor.
class WorkerGate {
public:
    explicit WorkerGate(JobSystem& jobs) : jobs_(jobs) {
        REQUIRE(jobs.worker_count() == 1);
        jobs_.run(
            [this] {
                std::unique_lock<std::mutex> lock(mu_);
                parked_ = true;
                cv_.notify_all();
                cv_.wait(lock, [this] { return open_; });
            },
            &joined_);
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return parked_; });
    }

    ~WorkerGate() { open(); }

    WorkerGate(const WorkerGate&) = delete;
    WorkerGate& operator=(const WorkerGate&) = delete;

    void open() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            open_ = true;
        }
        cv_.notify_all();
        jobs_.wait(joined_);
    }

private:
    JobSystem& jobs_;
    std::mutex mu_; // guards parked_ and open_
    std::condition_variable cv_;
    bool parked_ = false;
    bool open_ = false;
    JobSystem::Counter joined_{0};
};

} // namespace

TEST_CASE("streamed: request, ready, release — the slot and its payload are freed, counted once") {
    JobSystem jobs(2);
    AssetServer server(jobs);

    const HeightfieldAssetHandle h = server.request_heightfield(kTerrain);
    REQUIRE(h.is_valid());
    CHECK(server.state(h) == AssetState::Loading);
    CHECK(server.get(h) == nullptr);
    CHECK(server.live_heightfield_slots() == 1);

    // The job finishing parks the payload in the slot, but only pump() makes it Ready — the same
    // placeholder-until-pump contract the retained kinds have.
    server.wait_for_pending_loads();
    CHECK(server.resident_heightfields() == 1);
    CHECK(server.state(h) == AssetState::Loading);
    CHECK(server.get(h) == nullptr);
    CHECK(server.pump() == 1);
    CHECK(server.state(h) == AssetState::Ready);

    // The streamed bytes are the fixture's, exactly as the blocking reader decodes them.
    const HeightfieldAsset expected = sync_read_heightfield(kTerrain);
    const HeightfieldAsset* hf = server.get(h);
    REQUIRE(hf != nullptr);
    CHECK(hf->columns == 5);
    CHECK(hf->rows == 4);
    CHECK(hf->samples == expected.samples);
    CHECK_FALSE(hf->has_splat());

    // A terrain layer goes through the same machinery, in its own pool.
    const TerrainLayerAssetHandle l = server.request_terrain_layer(kLayer);
    settle(server);
    REQUIRE(server.get(l) != nullptr);
    CHECK(server.get(l)->material.value == 0x1f2e3d4c5b6a7988ull);
    CHECK(server.resident_terrain_layers() == 1);

    CHECK(server.stream_counters().evictions == 0);

    // Release the only owner: evicted now. Nothing occupied, nothing resident, one eviction.
    CHECK(server.release(h));
    CHECK(server.stream_counters().evictions == 1);
    CHECK(server.live_heightfield_slots() == 0);
    CHECK(server.resident_heightfields() == 0);
    CHECK(server.state(h) == AssetState::Stale);
    CHECK(server.get(h) == nullptr);
    CHECK(server.resident_terrain_layers() == 1); // the other pool is untouched

    CHECK(server.release(l));
    CHECK(server.stream_counters().evictions == 2);
    CHECK(server.live_terrain_layer_slots() == 0);
    CHECK(server.resident_terrain_layers() == 0);

    const StreamCounters c = server.stream_counters();
    CHECK(c.requests == 2);
    CHECK(c.loads_started == 2);
    CHECK(c.coalesced_requests == 0);
    CHECK(c.failed_loads == 0);
    CHECK(c.deferred_evictions == 0);
    CHECK(server.physical_load_count() == 2);
}

TEST_CASE("streamed: a stale handle never resolves to the index's new tenant") {
    JobSystem jobs(2);
    AssetServer server(jobs);

    const HeightfieldAssetHandle old_handle = server.request_heightfield(kTerrain);
    settle(server);
    REQUIRE(server.get(old_handle) != nullptr);
    REQUIRE(server.release(old_handle)); // evicted; index 0 goes on the free list

    // A DIFFERENT file now takes the same index — the collision the generation exists for. If
    // this precondition ever stops holding the test would pass vacuously, so it is REQUIREd.
    const HeightfieldAssetHandle tenant = server.request_heightfield(kTerrainSplat);
    settle(server);
    REQUIRE(tenant.index == old_handle.index);
    REQUIRE(tenant.generation != old_handle.generation);
    REQUIRE(server.get(tenant) != nullptr);
    REQUIRE(server.get(tenant)->has_splat());

    // The old handle sees nothing: not the splat terrain, not Ready, and each look is counted.
    const std::uint64_t stale_before = server.stream_counters().stale_handle_resolutions;
    CHECK(server.get(old_handle) == nullptr);
    CHECK(server.state(old_handle) == AssetState::Stale);
    CHECK(server.stream_counters().stale_handle_resolutions == stale_before + 2);

    // Nor can it release what it does not own: the tenant survives a stale release untouched.
    CHECK_FALSE(server.release(old_handle));
    CHECK(server.stream_counters().stale_handle_resolutions == stale_before + 3);
    CHECK(server.stream_counters().evictions == 1);
    CHECK(server.state(tenant) == AssetState::Ready);
    REQUIRE(server.get(tenant) != nullptr);
    CHECK(server.get(tenant)->weight_columns == 3);

    // An invalid (default) handle is "no handle": Failed like the retained kinds, and not counted
    // as stale.
    CHECK(server.state(HeightfieldAssetHandle{}) == AssetState::Failed);
    CHECK(server.get(HeightfieldAssetHandle{}) == nullptr);
    CHECK_FALSE(server.release(HeightfieldAssetHandle{}));
    CHECK(server.stream_counters().stale_handle_resolutions == stale_before + 3);

    CHECK(server.release(tenant));
    CHECK(server.live_heightfield_slots() == 0);
}

TEST_CASE("streamed: releasing while the load is in flight defers the eviction to the job") {
    JobSystem jobs(1);
    AssetServer server(jobs);
    WorkerGate gate(jobs); // the only worker is parked: nothing submitted below can run yet

    const HeightfieldAssetHandle h = server.request_heightfield(kTerrain);
    CHECK(server.state(h) == AssetState::Loading);
    CHECK(server.physical_load_count() == 0); // the job has provably not started

    // The last owner leaves while the job is still queued. Nothing may be freed: the job is going
    // to write into this slot by index.
    CHECK(server.release(h));
    StreamCounters c = server.stream_counters();
    CHECK(c.deferred_evictions == 1);
    CHECK(c.evictions == 0);
    CHECK(server.live_heightfield_slots() == 1); // still occupied — pinned by the job

    // The releaser's handle is dead already (it owns nothing), even though the slot is not.
    CHECK(server.state(h) == AssetState::Stale);

    // The pinned index must not be handed to another request. If it were, two load jobs would
    // publish into one slot.
    const HeightfieldAssetHandle other = server.request_heightfield(kTerrainSplat);
    CHECK(other.index != h.index);
    CHECK(server.live_heightfield_slots() == 2);

    // Let the jobs run. The first finds no owner and evicts — once.
    gate.open();
    server.wait_for_pending_loads();
    c = server.stream_counters();
    CHECK(c.evictions == 1);
    CHECK(c.deferred_evictions == 1);
    CHECK(c.cancelled_evictions == 0);
    CHECK(server.physical_load_count() == 2);
    CHECK(server.live_heightfield_slots() == 1);
    CHECK(server.resident_heightfields() == 1); // only `other`; the unwanted decode was dropped

    // The discarded load is not promoted: exactly one handle becomes Ready, and it is the right
    // file.
    CHECK(server.pump() == 1);
    CHECK(server.state(h) == AssetState::Stale);
    REQUIRE(server.get(other) != nullptr);
    CHECK(server.get(other)->has_splat());

    CHECK(server.release(other));
    CHECK(server.stream_counters().evictions == 2);
    CHECK(server.live_heightfield_slots() == 0);
    CHECK(server.resident_heightfields() == 0);
}

TEST_CASE("streamed: a request for a path whose eviction is owed adopts the in-flight load") {
    JobSystem jobs(1);
    AssetServer server(jobs);
    WorkerGate gate(jobs);

    const HeightfieldAssetHandle first = server.request_heightfield(kTerrain);
    CHECK(server.release(first)); // eviction owed
    CHECK(server.state(first) == AssetState::Stale);

    // Same path again before the job ran: join the load rather than read the file twice. The owed
    // eviction is cancelled, and that is counted rather than silently forgotten.
    const HeightfieldAssetHandle second = server.request_heightfield(kTerrain);
    CHECK(second == first); // same slot, same generation: it was never evicted
    StreamCounters c = server.stream_counters();
    CHECK(c.deferred_evictions == 1);
    CHECK(c.cancelled_evictions == 1);
    CHECK(c.coalesced_requests == 1);
    CHECK(c.loads_started == 1);

    gate.open();
    settle(server);
    c = server.stream_counters();
    CHECK(c.evictions == 0);
    CHECK(server.physical_load_count() == 1);
    CHECK(server.state(second) == AssetState::Ready);
    REQUIRE(server.get(second) != nullptr);

    CHECK(server.release(second));
    CHECK(server.stream_counters().evictions == 1);
}

TEST_CASE("streamed: two requests for one path are one load and two ownerships") {
    JobSystem jobs(2);
    AssetServer server(jobs);

    const TerrainLayerAssetHandle a = server.request_terrain_layer(kLayer);
    const TerrainLayerAssetHandle b = server.request_terrain_layer(kLayer);
    CHECK(a == b);
    settle(server);

    CHECK(server.physical_load_count() == 1);
    StreamCounters c = server.stream_counters();
    CHECK(c.requests == 2);
    CHECK(c.coalesced_requests == 1);
    CHECK(c.loads_started == 1);

    // One owner leaves: the other still holds it, so it stays resident and reachable.
    CHECK(server.release(a));
    CHECK(server.stream_counters().evictions == 0);
    CHECK(server.resident_terrain_layers() == 1);
    CHECK(server.state(b) == AssetState::Ready);
    REQUIRE(server.get(b) != nullptr);
    CHECK(server.get(b)->albedo_height.is_valid());

    // The second leaves: evicted.
    CHECK(server.release(b));
    CHECK(server.stream_counters().evictions == 1);
    CHECK(server.resident_terrain_layers() == 0);

    // A third release has no ownership behind it: refused and counted, not an underflow.
    const std::uint64_t stale_before = server.stream_counters().stale_handle_resolutions;
    CHECK_FALSE(server.release(b));
    CHECK(server.stream_counters().stale_handle_resolutions == stale_before + 1);
    CHECK(server.stream_counters().evictions == 1);

    // Re-requesting after eviction is a fresh physical load.
    const TerrainLayerAssetHandle again = server.request_terrain_layer(kLayer);
    settle(server);
    CHECK(server.physical_load_count() == 2);
    CHECK(server.state(again) == AssetState::Ready);
    CHECK(again.generation != b.generation);
    CHECK(server.release(again));
}

TEST_CASE("streamed: a release between the job finishing and pump() evicts and is not promoted") {
    JobSystem jobs(2);
    AssetServer server(jobs);

    const HeightfieldAssetHandle h = server.request_heightfield(kTerrain);
    server.wait_for_pending_loads(); // loaded and parked in the slot; not yet Ready
    REQUIRE(server.resident_heightfields() == 1);

    // Not in flight any more, so this is an immediate eviction, not a deferred one.
    CHECK(server.release(h));
    StreamCounters c = server.stream_counters();
    CHECK(c.evictions == 1);
    CHECK(c.deferred_evictions == 0);
    CHECK(server.resident_heightfields() == 0);

    // The index is reused before the pump that would have promoted the old load. That pump must
    // promote only the new tenant — once — and the new tenant must be the new file.
    const HeightfieldAssetHandle tenant = server.request_heightfield(kTerrainSplat);
    REQUIRE(tenant.index == h.index);
    server.wait_for_pending_loads();
    CHECK(server.pump() == 1);
    CHECK(server.state(h) == AssetState::Stale);
    REQUIRE(server.get(tenant) != nullptr);
    CHECK(server.get(tenant)->has_splat());
    CHECK(server.release(tenant));
}

TEST_CASE("streamed: a failed load is counted, stays Failed while owned, and retries after "
          "release") {
    TempDir tmp("rime_m19_8b_stream_fail");
    const fs::path corrupt = tmp.path / "corrupt.rhf";
    const std::vector<std::byte> junk(64, std::byte{0xAB});
    REQUIRE(rime::platform::write_file(corrupt, junk));
    const fs::path late = tmp.path / "late.rhf"; // does not exist yet

    JobSystem jobs(2);
    AssetServer server(jobs);

    const HeightfieldAssetHandle bad = server.request_heightfield(corrupt);
    const HeightfieldAssetHandle missing = server.request_heightfield(late);
    // A mesh file is a valid RMA1 container of the WRONG kind for this request.
    const TerrainLayerAssetHandle wrong_kind =
        server.request_terrain_layer(kFixtures / "quad.rmesh");
    const HeightfieldAssetHandle good = server.request_heightfield(kTerrain);
    settle(server);

    CHECK(server.stream_counters().failed_loads == 3);
    CHECK(server.state(bad) == AssetState::Failed);
    CHECK(server.state(missing) == AssetState::Failed);
    CHECK(server.state(wrong_kind) == AssetState::Failed);
    CHECK(server.get(bad) == nullptr);
    CHECK(server.state(good) == AssetState::Ready); // one bad file never poisons the others
    CHECK(server.resident_heightfields() == 1);
    CHECK(server.live_heightfield_slots() == 3); // a Failed slot is still an owned slot

    // While owned, a repeat request coalesces onto the Failed slot — no retry storm.
    const HeightfieldAssetHandle missing_again = server.request_heightfield(late);
    CHECK(missing_again == missing);
    CHECK(server.physical_load_count() == 4);

    // Releasing every owner evicts the Failed slot, and only THEN does a request try the file
    // again — by which time it may have appeared (a tile cooked late, a remount).
    CHECK(server.release(missing));
    CHECK(server.release(missing_again));
    CHECK(server.state(missing) == AssetState::Stale);
    const auto terrain_bytes = rime::platform::read_file(kTerrain);
    REQUIRE(terrain_bytes);
    REQUIRE(rime::platform::write_file(late, *terrain_bytes));
    const HeightfieldAssetHandle retried = server.request_heightfield(late);
    settle(server);
    CHECK(server.state(retried) == AssetState::Ready);
    CHECK(server.stream_counters().failed_loads == 3);

    CHECK(server.release(bad));
    CHECK(server.release(wrong_kind));
    CHECK(server.release(good));
    CHECK(server.release(retried));
    const StreamCounters c = server.stream_counters();
    CHECK(c.evictions == c.loads_started); // everything that was loaded has been freed
    CHECK(server.live_heightfield_slots() == 0);
    CHECK(server.live_terrain_layer_slots() == 0);
}

TEST_CASE("streamed: a load that fails with its eviction owed is counted as both") {
    TempDir tmp("rime_m19_8b_stream_fail_owed");
    JobSystem jobs(1);
    AssetServer server(jobs);
    WorkerGate gate(jobs);

    const HeightfieldAssetHandle h = server.request_heightfield(tmp.path / "nope.rhf");
    CHECK(server.release(h));
    gate.open();
    settle(server);

    const StreamCounters c = server.stream_counters();
    CHECK(c.failed_loads == 1);
    CHECK(c.deferred_evictions == 1);
    CHECK(c.evictions == 1);
    CHECK(server.live_heightfield_slots() == 0);
}

TEST_CASE("streamed: request/release churn from worker jobs conserves slots (TSan proof)") {
    // The threading brick. Worker jobs hammer K paths with requests, and half of them release at
    // once — so releases race load jobs (deferred evictions), requests race owed evictions
    // (cancellations) and indices are recycled under contention. Timing decides WHICH of those
    // paths each call takes, so the assertions are the conservation laws that hold whichever it
    // was, plus byte-exact content for everything still held.
    TempDir tmp("rime_m19_8b_stream_churn");
    const auto plain = rime::platform::read_file(kTerrain);
    const auto splat = rime::platform::read_file(kTerrainSplat);
    REQUIRE(plain);
    REQUIRE(splat);
    constexpr int kPaths = 6;
    std::vector<fs::path> paths;
    for (int k = 0; k < kPaths; ++k) {
        const fs::path p = tmp.path / ("tile" + std::to_string(k) + ".rhf");
        REQUIRE(rime::platform::write_file(p, k % 2 == 0 ? *plain : *splat)); // even = no splat
        paths.push_back(p);
    }

    JobSystem jobs(4);
    AssetServer server(jobs);

    constexpr int kN = 240;
    std::vector<HeightfieldAssetHandle> kept(kN); // each job writes only its own element
    JobSystem::Counter spawned{0};
    for (int i = 0; i < kN; ++i) {
        jobs.run(
            [&, i] {
                const HeightfieldAssetHandle h = server.request_heightfield(paths[i % kPaths]);
                if ((i / kPaths) % 2 == 1) {
                    server.release(h); // a transient owner: in and out
                } else {
                    kept[i] = h;
                }
            },
            &spawned);
    }
    jobs.wait(spawned);
    settle(server);

    StreamCounters c = server.stream_counters();
    CHECK(c.requests == kN);
    CHECK(c.requests == c.coalesced_requests + c.loads_started);
    CHECK(server.physical_load_count() == c.loads_started);
    // Every path has keepers, so exactly kPaths slots survive and every other load was evicted.
    CHECK(server.live_heightfield_slots() == kPaths);
    CHECK(server.resident_heightfields() == kPaths);
    CHECK(c.evictions == c.loads_started - kPaths);
    CHECK(c.cancelled_evictions <= c.deferred_evictions);
    CHECK(c.failed_loads == 0);

    // Every kept handle is live, Ready, and holds ITS path's bytes — not a neighbour's that
    // happened to recycle the index.
    for (int i = 0; i < kN; ++i) {
        if (!kept[i].is_valid()) {
            continue;
        }
        const HeightfieldAsset* hf = server.get(kept[i]);
        REQUIRE(hf != nullptr);
        CHECK(hf->has_splat() == ((i % kPaths) % 2 == 1));
    }
    CHECK(server.stream_counters().stale_handle_resolutions == c.stale_handle_resolutions);

    // Release every keeper: the server ends empty, and evictions account for every load started.
    for (int i = 0; i < kN; ++i) {
        if (kept[i].is_valid()) {
            CHECK(server.release(kept[i]));
        }
    }
    c = server.stream_counters();
    CHECK(c.evictions == c.loads_started);
    CHECK(server.live_heightfield_slots() == 0);
    CHECK(server.resident_heightfields() == 0);
}
