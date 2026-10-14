// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "rime/assets/procedural_terrain.hpp"
#include "rime/physics/query.hpp"
#include "rime/physics/world.hpp"
#include "rime/terrain_collision/procedural_source.hpp"
#include "rime/terrain_collision/terrain_collision.hpp"

// The procedural terrain source: a pure generator (assets/procedural_terrain.hpp) and the
// TileSource that serves it. Headless, GPU-free, no threads, no files.
//
// What these proofs are NOT: they are not a claim the hills look good. They establish the
// properties the rest of the engine leans on — every sample a pure function of (seed, version,
// global integer metre), so tiles and LODs can never disagree about shared ground; a flat street
// that sits strictly below the slab; and a source whose every failure is a counted, visible one
// rather than a quietly empty tile.
using namespace rime;
namespace tc = rime::terrain_collision;

namespace {

using assets::ProceduralTerrainConfig;

// Heights in quantisation steps. -1.5 m, 0 m and the top of the hills.
constexpr std::uint16_t kStreetStep = 640;
constexpr std::uint16_t kGroundStep = 1024;
constexpr std::uint16_t kTopStep = 1024 + 8192;

// FNV-1a over the little-endian bytes of each sample: a hash with a specification short enough to
// restate here, so a golden value is checkable by reading, not by trusting a library.
void fnv(std::uint64_t& h, std::uint16_t v) {
    for (int b = 0; b < 2; ++b) {
        h ^= static_cast<std::uint8_t>((v >> (8 * b)) & 0xffu);
        h *= 0x100000001b3ull;
    }
}

[[nodiscard]] std::uint64_t golden_hash(const ProceduralTerrainConfig& config) {
    struct Probe {
        std::int32_t x, z;
        std::uint32_t lod;
    };

    const Probe probes[] = {{0, 0, 0}, {-1, -1, 0}, {3, -2, 0}, {-5, 7, 0}, {1, -1, 3}};
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const Probe& p : probes) {
        const auto tile = assets::generate_procedural_tile(config, p.x, p.z, p.lod);
        REQUIRE(tile.has_value());
        for (const std::uint16_t s : tile->samples) {
            fnv(h, s);
        }
    }
    return h;
}

[[nodiscard]] assets::HeightfieldAsset
tile(const ProceduralTerrainConfig& config, std::int32_t x, std::int32_t z, std::uint32_t lod = 0) {
    const auto t = assets::generate_procedural_tile(config, x, z, lod);
    REQUIRE(t.has_value());
    return *t;
}

[[nodiscard]] tc::ProceduralTileSource::Options options(std::uint32_t slots) {
    tc::ProceduralTileSource::Options o;
    o.max_slots = slots;
    return o;
}

[[nodiscard]] std::int32_t floor_div(std::int32_t v, std::int32_t d) {
    std::int32_t q = v / d;
    if (v % d != 0 && ((v < 0) != (d < 0))) {
        --q;
    }
    return q;
}

} // namespace

TEST_CASE("procedural terrain: a fixed seed and version give fixed samples, and the seed is what "
          "they depend on") {
    const ProceduralTerrainConfig config;
    const std::uint64_t golden = golden_hash(config);
    // Measured, not derived. If a deliberate change to the generator moves this, the new number
    // goes here with the reason in the commit; an accidental move is the bug this exists to catch.
    CHECK(golden == 0x78b019ad70ce8ac3ull);
    CHECK(golden_hash(config) == golden); // pure: asking twice is not a different answer

    // SENSITIVITY CONTROL. A hash that did not move with the seed would pass the line above for a
    // generator that ignored its seed entirely.
    ProceduralTerrainConfig other = config;
    other.seed ^= 1;
    CHECK(golden_hash(other) != golden);
    // The street footprint is part of the function too: moving it moves the samples.
    ProceduralTerrainConfig moved = config;
    moved.street_max_x += 2;
    CHECK(golden_hash(moved) != golden);
}

TEST_CASE("procedural terrain: invalid inputs are refused, never returned as flat ground") {
    const ProceduralTerrainConfig config;
    CHECK(assets::generate_procedural_tile(config, 0, 0, 0).has_value());
    CHECK_FALSE(assets::generate_procedural_tile(config, 0, 0, assets::kProceduralMaxLod + 1));
    CHECK_FALSE(assets::generate_procedural_tile(config, 0, 0, 0xffffffffu));

    ProceduralTerrainConfig bad = config;
    bad.version = 2;
    CHECK_FALSE(assets::generate_procedural_tile(bad, 0, 0, 0));
    CHECK_FALSE(assets::procedural_terrain_sample(bad, 0, 0));
    bad = config;
    bad.version = 0;
    CHECK_FALSE(assets::generate_procedural_tile(bad, 0, 0, 0));
    bad = config;
    bad.street_min_x = bad.street_max_x + 1; // inverted
    CHECK_FALSE(assets::generate_procedural_tile(bad, 0, 0, 0));
    bad = config;
    bad.street_max_z = INT32_MAX; // beyond what a float names exactly
    CHECK_FALSE(assets::generate_procedural_tile(bad, 0, 0, 0));

    // The finite edge of the world, at both ends and at int32's own extremes (no overflow, no UB).
    CHECK(assets::procedural_tile_supported(262143, -262144, 0));
    CHECK_FALSE(assets::procedural_tile_supported(262144, 0, 0));
    CHECK_FALSE(assets::procedural_tile_supported(0, -262145, 0));
    CHECK(assets::procedural_tile_supported(1023, -1024, assets::kProceduralMaxLod));
    CHECK_FALSE(assets::procedural_tile_supported(1024, 0, assets::kProceduralMaxLod));
    CHECK_FALSE(assets::procedural_tile_supported(INT32_MAX, 0, 0));
    CHECK_FALSE(assets::procedural_tile_supported(INT32_MIN, INT32_MIN, 0));
    CHECK_FALSE(assets::generate_procedural_tile(config, INT32_MAX, INT32_MAX, 0));
    CHECK_FALSE(assets::generate_procedural_tile(config, INT32_MIN, 0, assets::kProceduralMaxLod));
    CHECK_FALSE(assets::procedural_terrain_sample(config, std::int64_t{1} << 41, 0));
}

TEST_CASE("procedural terrain: adjacent tiles agree exactly on their shared edge, across zero and "
          "at every LOD") {
    const ProceduralTerrainConfig config;
    const std::uint32_t last = assets::kProceduralTileCells;
    for (const std::uint32_t lod : {0u, 1u, 3u, assets::kProceduralMaxLod}) {
        for (const std::int32_t base : {-3, -1, 0, 5}) {
            // Positive and negative x/z neighbours, including the pair that straddles the origin.
            const assets::HeightfieldAsset left = tile(config, base, -1, lod);
            const assets::HeightfieldAsset right = tile(config, base + 1, -1, lod);
            const assets::HeightfieldAsset below = tile(config, base, -2, lod);
            const assets::HeightfieldAsset above =
                tile(config, base, -1, lod); // z = -1 over z = -2
            for (std::uint32_t k = 0; k <= last; ++k) {
                CHECK(left.samples[left.index(last, k)] == right.samples[right.index(0, k)]);
                CHECK(below.samples[below.index(k, last)] == above.samples[above.index(k, 0)]);
            }
            // The geometry says the same thing the samples do: the right tile starts where the
            // left one ends.
            CHECK(right.origin.x == left.origin.x + static_cast<float>(last) * left.cell_size_x);
            CHECK(above.origin.z == below.origin.z + static_cast<float>(last) * below.cell_size_z);
        }
    }
}

TEST_CASE("procedural terrain: a coarse tile is a subsample of the finer LODs, and every sample is "
          "the global function") {
    const ProceduralTerrainConfig config;
    const std::uint32_t cells = assets::kProceduralTileCells;
    for (const std::int32_t tx : {-2, -1, 0, 1}) {
        for (const std::int32_t tz : {-1, 0, 2}) {
            const assets::HeightfieldAsset coarse = tile(config, tx, tz, 1);
            const std::array<assets::HeightfieldAsset, 4> fine_tiles{
                tile(config, 2 * tx, 2 * tz),
                tile(config, 2 * tx + 1, 2 * tz),
                tile(config, 2 * tx, 2 * tz + 1),
                tile(config, 2 * tx + 1, 2 * tz + 1)};
            // Four fine tiles cover it; coarse sample (i, j) is fine sample (2i, 2j) of the one
            // that holds that global position (the far edge belongs to the last tile's edge).
            for (std::uint32_t j = 0; j <= cells; ++j) {
                for (std::uint32_t i = 0; i <= cells; ++i) {
                    const std::uint32_t gi = 2 * i;
                    const std::uint32_t gj = 2 * j;
                    const std::uint32_t oi = std::min(gi / cells, 1u);
                    const std::uint32_t oj = std::min(gj / cells, 1u);
                    const assets::HeightfieldAsset& fine = fine_tiles[oi + 2 * oj];
                    REQUIRE(coarse.samples[coarse.index(i, j)] ==
                            fine.samples[fine.index(gi - oi * cells, gj - oj * cells)]);
                }
            }
        }
    }
    // And each sample of a high-LOD tile is the pure function at its global metre.
    const assets::HeightfieldAsset far = tile(config, -7, 4, 5);
    for (std::uint32_t j = 0; j <= cells; ++j) {
        for (std::uint32_t i = 0; i <= cells; ++i) {
            const std::int64_t x = -7 * (64 << 5) + std::int64_t{i} * (2 << 5);
            const std::int64_t z = 4 * (64 << 5) + std::int64_t{j} * (2 << 5);
            CHECK(far.samples[far.index(i, j)] == *assets::procedural_terrain_sample(config, x, z));
        }
    }
}

TEST_CASE("procedural terrain: the street is flat at -1.5 m in every tile that touches it") {
    const ProceduralTerrainConfig c;
    const std::int32_t tx0 = floor_div(c.street_min_x, 64), tx1 = floor_div(c.street_max_x, 64);
    const std::int32_t tz0 = floor_div(c.street_min_z, 64), tz1 = floor_div(c.street_max_z, 64);
    REQUIRE(tx0 < 0);
    REQUIRE(tz0 < 0); // the street straddles the origin: at least four tiles touch it
    std::uint64_t inside = 0;
    for (std::int32_t tz = tz0; tz <= tz1; ++tz) {
        for (std::int32_t tx = tx0; tx <= tx1; ++tx) {
            const assets::HeightfieldAsset t = tile(c, tx, tz);
            for (std::uint32_t j = 0; j < t.rows; ++j) {
                for (std::uint32_t i = 0; i < t.columns; ++i) {
                    const std::int64_t x = std::int64_t{tx} * 64 + 2 * i;
                    const std::int64_t z = std::int64_t{tz} * 64 + 2 * j;
                    if (x >= c.street_min_x && x <= c.street_max_x && z >= c.street_min_z &&
                        z <= c.street_max_z) {
                        ++inside;
                        CHECK(t.samples[t.index(i, j)] == kStreetStep);
                        CHECK(t.height(i, j) == -1.5f); // exact: scale and offset are binary
                    } else {
                        CHECK(t.samples[t.index(i, j)] > kStreetStep); // never below the street
                    }
                }
            }
        }
    }
    // A street 76 x 76 m is 39 x 39 samples at 2 m, give or take the inclusive edges: the loop
    // really did land samples inside, so the equalities above were not vacuous.
    CHECK(inside >= 38 * 38);
    // The slab occupies y in [-1, 0]; -1.5 m leaves half a metre of air under it.
    CHECK(-1.5f < -1.0f);
}

TEST_CASE("procedural terrain: bands are continuous, the hold band is exactly flat, and no sample "
          "overflows") {
    const ProceduralTerrainConfig c;
    // March outward from the +X street edge, and from the corner along the diagonal (where the
    // Euclidean distance, not either axis, sets the band).
    for (const int dir : {0, 1}) {
        std::optional<std::uint16_t> previous;
        for (std::int64_t m = 0; m <= 400; ++m) {
            const std::int64_t x = c.street_max_x + m;
            const std::int64_t z = dir == 0 ? 0 : c.street_max_z + m;
            const std::uint16_t s = *assets::procedural_terrain_sample(c, x, z);
            if (previous) {
                const int step = std::abs(static_cast<int>(s) - static_cast<int>(*previous));
                // Worst case from the three octaves and the two smoothsteps: ~340 steps/m of
                // noise gradient per axis plus ~190 from the hill blend, so ~540 along an axis
                // and ~950 on the diagonal. A missing fade or a seam is a jump of thousands.
                CHECK(step <= 1000);
            }
            previous = s;
            CHECK(s >= kStreetStep);
            CHECK(s <= kTopStep);
            if (dir == 0 && m >= 12 && m <= 76) {
                CHECK(s == kGroundStep); // the outer hold band
            }
        }
        // The blend starts at the street level and ends at ground, monotonically.
        std::uint16_t last = 0;
        for (std::int64_t m = 0; m <= 12; ++m) {
            const std::int64_t x = c.street_max_x + m;
            const std::int64_t z = dir == 0 ? 0 : c.street_max_z + m;
            const std::uint16_t s = *assets::procedural_terrain_sample(c, x, z);
            CHECK(s >= last);
            last = s;
        }
        CHECK(last == kGroundStep);
    }

    // Extrema over a wide region at the supported edge of the world: in range, and the hills
    // actually express their range (control: a generator that returned 1024 everywhere would pass
    // every bound above).
    std::uint16_t lo = 0xffff, hi = 0;
    for (const std::int32_t tx : {-262144, -262143, 262142, 262143}) {
        const assets::HeightfieldAsset t = tile(c, tx, tx < 0 ? 262143 : -262144);
        lo = std::min(lo, t.min_sample);
        hi = std::max(hi, t.max_sample);
        CHECK(t.min_sample == *std::min_element(t.samples.begin(), t.samples.end()));
        CHECK(t.max_sample == *std::max_element(t.samples.begin(), t.samples.end()));
    }
    CHECK(lo >= kGroundStep);
    CHECK(hi <= kTopStep);
    for (std::int32_t k = -40; k < 40; ++k) {
        const assets::HeightfieldAsset t = tile(c, 100 + k, 100 - k);
        lo = std::min(lo, t.min_sample);
        hi = std::max(hi, t.max_sample);
    }
    CHECK(hi - lo > 4096); // at least 16 m of relief somewhere in the sample
}

TEST_CASE("procedural source: a full pool is a visible refusal, never an absence of terrain") {
    tc::ProceduralTileSource source(options(3));
    std::vector<assets::HeightfieldAssetHandle> held;
    for (std::int32_t x = 0; x < 3; ++x) {
        const auto h = source.request({x, 0, 1});
        REQUIRE(h.is_valid());
        CHECK(source.state(h) == assets::AssetState::Ready);
        held.push_back(h);
    }
    CHECK(source.stats().live_payloads == 3);

    const auto refused = source.request({9, 9, 1});
    CHECK_FALSE(refused.is_valid());
    CHECK(source.state(refused) == assets::AssetState::Failed); // reads as a failed load
    CHECK(source.stats().refused_requests == 1);
    CHECK(source.stats().refused_pool_full == 1);
    CHECK(source.stats().refused_unsupported == 0);
    CHECK(source.stats().live_payloads == 3); // the refused request took nothing

    // The manifest question has the same answer it had when the pool was empty.
    std::uint32_t revision = 0;
    CHECK(source.current_revision(9, 9, revision));
    CHECK(revision == 1);

    // Giving one back makes room again — no permanent damage from the refusal.
    REQUIRE(source.release(held[1]));
    const auto again = source.request({9, 9, 1});
    CHECK(again.is_valid());
    CHECK(source.stats().peak_payloads == 3);
}

TEST_CASE("procedural source: unsupported version, revision and coordinates are counted failures") {
    ProceduralTerrainConfig future;
    future.version = 7;
    tc::ProceduralTileSource::Options o;
    o.config = future;
    tc::ProceduralTileSource wrong_version(o);
    std::uint32_t revision = 0;
    // Not "no terrain": the manifest still names a revision, and the request is what fails.
    CHECK(wrong_version.current_revision(0, 0, revision));
    CHECK_FALSE(wrong_version.request({0, 0, 7}).is_valid());
    CHECK(wrong_version.stats().refused_unsupported == 1);

    tc::ProceduralTileSource source(options(4));
    CHECK_FALSE(source.request({0, 0, 2}).is_valid()); // not the configured revision
    CHECK_FALSE(source.request({262144, 0, 1}).is_valid());
    CHECK_FALSE(source.request({0, INT32_MIN, 1}).is_valid());
    CHECK(source.stats().refused_unsupported == 3);
    CHECK(source.stats().refused_pool_full == 0);
    CHECK(source.stats().live_payloads == 0);
    // Past the edge of the finite world there genuinely is no terrain.
    CHECK_FALSE(source.current_revision(262144, 0, revision));
    CHECK(source.current_revision(262143, -262144, revision));
}

TEST_CASE("procedural source: a handle kept past its release is stale, even after the slot is "
          "reused") {
    tc::ProceduralTileSource source(options(1)); // one slot: reuse is guaranteed
    const auto first = source.request({0, 0, 1});
    REQUIRE(first.is_valid());
    const assets::HeightfieldAsset* payload = source.resolve(first).asset;
    REQUIRE(payload != nullptr);
    CHECK(source.resolve(first).revision == 1);
    REQUIRE(source.release(first));
    CHECK(source.state(first) == assets::AssetState::Stale);
    CHECK(source.resolve(first).asset == nullptr);

    const auto second = source.request({5, -5, 1});
    REQUIRE(second.is_valid());
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    CHECK_FALSE(first == second);
    // The old handle neither reads the new tenant nor releases it.
    CHECK(source.state(first) == assets::AssetState::Stale);
    CHECK(source.resolve(first).asset == nullptr);
    CHECK_FALSE(source.release(first));
    CHECK(source.stats().stale_releases == 1);
    REQUIRE(source.resolve(second).asset != nullptr);
    CHECK(source.resolve(second).asset->origin.x == 5 * 64.0f);
    CHECK(source.stats().live_payloads == 1);
    // A second release of the current handle is also refused, not a second free.
    REQUIRE(source.release(second));
    CHECK_FALSE(source.release(second));
    CHECK(source.stats().live_payloads == 0);
}

TEST_CASE("procedural source: thousands of unique tile cycles keep slots and payload memory "
          "bounded") {
    constexpr std::uint32_t kSlots = 8;
    constexpr std::uint32_t kWindow = 3; // tiles held at once, like a moving demand
    tc::ProceduralTileSource source(options(kSlots));
    const std::uint64_t tile_bytes = std::uint64_t{33} * 33 * sizeof(std::uint16_t);
    std::vector<assets::HeightfieldAssetHandle> window;
    std::uint64_t cycles = 0;
    for (std::int32_t z = -40; z < 40; ++z) {
        for (std::int32_t x = -40; x < 40; ++x) { // 6400 unique coordinates
            const auto h = source.request({x, z, 1});
            REQUIRE(h.is_valid());
            ++cycles;
            window.push_back(h);
            if (window.size() > kWindow) {
                REQUIRE(source.release(window.front()));
                window.erase(window.begin());
            }
            REQUIRE(source.stats().live_payloads <= kWindow);
            REQUIRE(source.stats().payload_sample_bytes <= kWindow * tile_bytes);
        }
    }
    for (const auto h : window) {
        REQUIRE(source.release(h));
    }
    const auto& s = source.stats();
    CHECK(cycles == 6400);
    CHECK(s.generated == cycles);
    CHECK(s.releases == cycles);
    CHECK(s.live_payloads == 0);
    CHECK(s.payload_sample_bytes == 0);
    CHECK(s.peak_payloads == kWindow + 1); // the new request lands before the oldest is released
    CHECK(s.peak_payload_sample_bytes == (kWindow + 1) * tile_bytes);
    CHECK(s.refused_requests == 0);
    CHECK(source.max_slots() == kSlots); // the slot table never grew
}

TEST_CASE("procedural source: two independent sources with one config serve identical samples") {
    tc::ProceduralTileSource a(options(4));
    tc::ProceduralTileSource b(options(4));
    for (const std::int32_t x : {-9, -1, 0, 4}) {
        const auto ha = a.request({x, -x, 1});
        // Different request history on b: the handle values may differ, the samples may not.
        const auto filler = b.request({x + 100, 0, 1});
        const auto hb = b.request({x, -x, 1});
        REQUIRE(ha.is_valid());
        REQUIRE(hb.is_valid());
        const assets::HeightfieldAsset* pa = a.resolve(ha).asset;
        const assets::HeightfieldAsset* pb = b.resolve(hb).asset;
        REQUIRE(pa != nullptr);
        REQUIRE(pb != nullptr);
        CHECK(pa->samples == pb->samples);
        CHECK(pa->origin.x == pb->origin.x);
        CHECK(pa->samples == tile(ProceduralTerrainConfig{}, x, -x).samples);
        REQUIRE(a.release(ha));
        REQUIRE(b.release(hb));
        REQUIRE(b.release(filler));
    }
}

TEST_CASE("procedural source: TerrainCollision installs and evicts procedural tiles as real "
          "ground") {
    tc::ProceduralTileSource source(options(32));
    tc::Config config; // origin 0, 64 m tiles: exactly the generator's LOD 0 grid
    config.tile_size_x = 64.0f;
    config.tile_size_z = 64.0f;
    physics::PhysicsWorld world;
    {
        tc::TerrainCollision terrain(config, source, world);
        // A spot over the middle of the street.
        physics::Aabb spot;
        spot.min = {9.0f, -2.0f, 9.0f};
        spot.max = {11.0f, 2.0f, 11.0f};
        REQUIRE(terrain.request_admission(1, spot) == tc::AdmissionResult::Deferred);

        const std::uint64_t lead = terrain.config().activation_lead_ticks;
        REQUIRE(terrain.try_commit(terrain.plan(0, {})) == tc::CommitStatus::Ready);
        // Synchronous source: every requested tile is Ready now, and the lead — not the load —
        // is what still holds the install back.
        CHECK(terrain.tile_state({0, 0, 1}) == tc::TileState::Requested);
        CHECK(source.stats().live_payloads > 0);
        CHECK(world.body_count() == 0);
        for (std::uint64_t tick = 1; tick <= lead; ++tick) {
            REQUIRE(terrain.try_commit(terrain.plan(tick, {})) == tc::CommitStatus::Ready);
        }
        CHECK(terrain.tile_state({0, 0, 1}) == tc::TileState::Installed);
        CHECK(terrain.counters().load_failures == 0);
        CHECK(terrain.counters().install_late == 0);
        // Physics holds its own copy and the handles went straight back.
        CHECK(source.stats().live_payloads == 0);
        CHECK(source.stats().payload_sample_bytes == 0);
        CHECK(world.body_count() == terrain.counters().tiles_installed);
        CHECK(terrain.counters().bytes_resident ==
              terrain.counters().tiles_installed * 33 * 33 * sizeof(std::uint16_t));

        // The installed ground is the street: a ray from above lands at -1.5 m.
        physics::Ray ray;
        ray.origin = {10.0f, 50.0f, 10.0f};
        ray.direction = {0.0f, -1.0f, 0.0f};
        ray.max_distance = 200.0f;
        physics::RayHit hit;
        REQUIRE(world.raycast(ray, hit));
        CHECK(hit.point.y == doctest::Approx(-1.5f).epsilon(0.001));

        // Nothing demands it any more: the tiles go, and the source stays empty.
        const std::uint64_t installed = terrain.counters().tiles_installed;
        REQUIRE(terrain.try_commit(terrain.plan(lead + 1, {})) == tc::CommitStatus::Ready);
        CHECK(world.body_count() == 0);
        CHECK(terrain.counters().evictions == installed);
        CHECK(terrain.tile_state({0, 0, 1}) == tc::TileState::Absent);
        CHECK_FALSE(world.raycast(ray, hit));
        CHECK(source.stats().live_payloads == 0);
        CHECK(source.stats().refused_requests == 0);
    }
    CHECK(source.stats().generated == source.stats().releases);
}
