// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m19.8d1 — the level-aware terrain world (ADR-0070). CPU-only: levelled manifest lines (and the
// m19.8a line still reading as level 0), the chain's shape checks, check_tile at a level (spacing
// 2^L, one quantisation, dyadic placement), the coincident-sample invariant, verify_lod_chain over
// clean and doctored chains, and the Rust-cooked `lod_world` fixture verified end to end with its
// geometric error recomputed here, independently, from the cooked payloads.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/assets/terrain_world.hpp"
#include "rime/assets/texture_asset.hpp"
#include "rime/platform/filesystem.hpp"

namespace {

using rime::assets::check_tile;
using rime::assets::HeightfieldAsset;
using rime::assets::samples_coincide;
using rime::assets::TerrainTileCheck;
using rime::assets::TerrainTileCoord;
using rime::assets::TerrainTileKey;
using rime::assets::TerrainWorld;
using rime::assets::TerrainWorldGrid;
using rime::assets::TerrainWorldRefusals;
using rime::assets::TerrainWorldTile;

TerrainWorldGrid grid5() {
    TerrainWorldGrid g{};
    g.samples = 5;
    g.cell_size_x = 1.0f;
    g.cell_size_z = 2.0f;
    g.height_scale = 0.01f;
    g.height_offset = -2.0f;
    g.origin = {100.0f, 5.0f, -40.0f};
    return g;
}

// A deterministic "random" integer field over GLOBAL level-0 sample coordinates: tiles cut from it
// agree on shared edges, and a parent cut from it at stride 2^L is the nested subsample by
// definition — an independent construction from the cook's child-by-child copy.
std::uint16_t field(std::int64_t gx, std::int64_t gz) {
    auto h =
        static_cast<std::uint64_t>(gx * 73856093LL) ^ static_cast<std::uint64_t>(gz * 19349663LL);
    h ^= h >> 13;
    h *= 0x5bd1e995ULL;
    h ^= h >> 15;
    return static_cast<std::uint16_t>(1000 + h % 50000);
}

HeightfieldAsset tile_at(const TerrainWorldGrid& g, TerrainTileKey k) {
    HeightfieldAsset a{};
    a.columns = g.samples;
    a.rows = g.samples;
    a.cell_size_x = g.cell_size_x_at(k.level);
    a.cell_size_z = g.cell_size_z_at(k.level);
    a.height_scale = g.height_scale;
    a.height_offset = g.height_offset;
    a.origin = g.tile_origin(k);
    const std::int64_t n = g.samples - 1;
    const std::int64_t side = std::int64_t{1} << k.level;
    for (std::uint32_t j = 0; j < g.samples; ++j) {
        for (std::uint32_t i = 0; i < g.samples; ++i) {
            a.samples.push_back(field((k.coord.x * n + i) * side, (k.coord.z * n + j) * side));
        }
    }
    a.min_sample = *std::min_element(a.samples.begin(), a.samples.end());
    a.max_sample = *std::max_element(a.samples.begin(), a.samples.end());
    return a;
}

std::string tile_line(TerrainTileKey k) {
    return "tile\t" + std::to_string(k.level) + "\t" + std::to_string(k.coord.x) + "\t" +
           std::to_string(k.coord.z) + "\t0\t-1\t9\t0.5\t0\tt.rhf\n";
}

constexpr const char* kGridLine = "grid\t5\t1\t2\t0.01\t-2\t100\t5\t-40\n";

// A 2x2 world plus its one parent, as a manifest.
std::string chain_2x2() {
    std::string s = kGridLine;
    for (std::int32_t z = 0; z < 2; ++z) {
        for (std::int32_t x = 0; x < 2; ++x) {
            s += tile_line({0, {x, z}});
        }
    }
    return s + tile_line({1, {0, 0}});
}

// A world built in memory: every key's tile from the field, a loader that serves them by path.
struct MemoryWorld {
    TerrainWorld world;
    std::map<std::string, HeightfieldAsset> files;

    [[nodiscard]] std::optional<HeightfieldAsset> load(const TerrainWorldTile& t) const {
        const auto it = files.find(t.path);
        return it != files.end() ? std::optional<HeightfieldAsset>(it->second) : std::nullopt;
    }
};

std::string key_path(TerrainTileKey k) {
    return "L" + std::to_string(k.level) + "_" + std::to_string(k.coord.x) + "_" +
           std::to_string(k.coord.z) + ".rhf";
}

MemoryWorld memory_world(std::int32_t tiles_per_side, std::uint32_t levels) {
    auto made = TerrainWorld::make(grid5());
    REQUIRE(made.has_value());
    MemoryWorld m{std::move(*made), {}};
    for (std::uint32_t l = 0; l < levels; ++l) {
        const std::int32_t n = tiles_per_side >> l;
        for (std::int32_t z = 0; z < n; ++z) {
            for (std::int32_t x = 0; x < n; ++x) {
                const TerrainTileKey k{l, {x, z}};
                TerrainWorldTile t{};
                t.coord = k.coord;
                t.level = l;
                t.path = key_path(k);
                REQUIRE(m.world.add_tile(t));
                m.files[t.path] = tile_at(grid5(), k);
            }
        }
    }
    REQUIRE(m.world.validate_levels());
    return m;
}

} // namespace

TEST_CASE("m19.8d1: levelled tile lines parse, and an m19.8a line still reads as level 0") {
    // Mixed on purpose: three 10-field lines, one 8-field (m19.8a) line, one parent.
    std::string text = kGridLine;
    text += "tile\t0\t0\t0\t3\t-1\t9\t0\t1f\ta.rhf\n";
    text += "tile\t1\t0\t3\t-1\t9\t1f\tb.rhf\n"; // 8 fields: level 0 tile (1, 0)
    text += "tile\t0\t0\t1\t0\t-1\t9\t0\t0\tc.rhf\n";
    text += "tile\t0\t1\t1\t0\t-1\t9\t0\t0\td.rhf\n";
    text += "tile\t1\t0\t0\t7\t-1.5\t12.25\t0.375\tabc\tparent.rhf\n";
    const auto w = TerrainWorld::parse(text);
    REQUIRE(w.has_value());
    CHECK(w->level_count() == 2);
    CHECK(w->tiles().size() == 4);
    CHECK(w->tiles(1).size() == 1);
    CHECK(w->tiles(7).empty());
    const TerrainWorldTile* old = w->find(TerrainTileCoord{1, 0});
    REQUIRE(old != nullptr);
    CHECK(old->level == 0);
    CHECK(old->geometric_error == 0.0f);
    CHECK(old->path == "b.rhf");
    const TerrainWorldTile* p = w->find(TerrainTileKey{1, {0, 0}});
    REQUIRE(p != nullptr);
    CHECK(p->revision == 7);
    CHECK(p->min_y == -1.5f);
    CHECK(p->max_y == 12.25f);
    CHECK(p->geometric_error == 0.375f);
    CHECK(p->id.value == 0xabcU);
    CHECK(p->path == "parent.rhf");
    CHECK(w->find(TerrainTileKey{1, {1, 0}}) == nullptr);
    CHECK(w->refusals().missing_children == 0);
    CHECK(w->refusals().uncovered_tiles == 0);

    // Malformed levelled lines refuse the file, as any malformed line does.
    const auto bad = [](const char* line) {
        return !TerrainWorld::parse(std::string(kGridLine) + line).has_value();
    };
    CHECK(bad("tile\t17\t0\t0\t0\t-1\t9\t0\t0\ta.rhf\n"));   // beyond kMaxTerrainLevel
    CHECK(bad("tile\t1\t0\t0\t0\t-1\t9\t-0.5\t0\ta.rhf\n")); // negative error
    CHECK(bad("tile\t1\t0\t0\t0\t-1\t9\tnan\t0\ta.rhf\n"));
    CHECK(bad("tile\t1\t0\t0\t0\t-1\t9\t0\ta.rhf\n"));     // 9 fields
    CHECK(bad("tile\t-1\t0\t0\t0\t-1\t9\t0\t0\ta.rhf\n")); // a level is unsigned
}

TEST_CASE("m19.8d1: validate_levels refuses a chain with a missing child, an uncovered tile, or an "
          "odd sample count — as a unit, counted, level 0 kept") {
    SUBCASE("complete chain") {
        const auto w = TerrainWorld::parse(chain_2x2());
        REQUIRE(w.has_value());
        CHECK(w->level_count() == 2);
    }
    SUBCASE("missing child") {
        std::string text = kGridLine;
        text += tile_line({0, {0, 0}}) + tile_line({0, {1, 0}}) + tile_line({0, {0, 1}});
        text += tile_line({1, {0, 0}});
        const auto w = TerrainWorld::parse(text);
        REQUIRE(w.has_value());
        CHECK(w->refusals().missing_children == 1);
        CHECK(w->refusals().uncovered_tiles == 0);
        CHECK(w->level_count() == 1);  // the chain is gone...
        CHECK(w->tiles().size() == 3); // ...level 0 is not
        CHECK(w->find(TerrainTileKey{1, {0, 0}}) == nullptr);
    }
    SUBCASE("incomplete root cover") {
        std::string text = chain_2x2();
        for (std::int32_t z = 0; z < 2; ++z) {
            text += tile_line({0, {2, z}}) + tile_line({0, {3, z}}); // no parent (1, 0)
        }
        const auto w = TerrainWorld::parse(text);
        REQUIRE(w.has_value());
        CHECK(w->refusals().uncovered_tiles == 4);
        CHECK(w->refusals().missing_children == 0);
        CHECK(w->level_count() == 1);
        CHECK(w->tiles().size() == 8);
    }
    SUBCASE("a skipped level leaves every parent childless and every leaf uncovered") {
        std::string text = kGridLine;
        for (std::int32_t z = 0; z < 4; ++z) {
            for (std::int32_t x = 0; x < 4; ++x) {
                text += tile_line({0, {x, z}});
            }
        }
        text += tile_line({2, {0, 0}});
        const auto w = TerrainWorld::parse(text);
        REQUIRE(w.has_value());
        CHECK(w->refusals().missing_children == 1);
        CHECK(w->refusals().uncovered_tiles == 16);
        CHECK(w->level_count() == 1);
    }
    SUBCASE("odd cell count") {
        std::string text = chain_2x2();
        text.replace(0, std::string(kGridLine).size(), "grid\t4\t1\t2\t0.01\t-2\t100\t5\t-40\n");
        const auto w = TerrainWorld::parse(text);
        REQUIRE(w.has_value());
        CHECK(w->refusals().odd_lod_samples == 1);
        CHECK(w->level_count() == 1);
    }
    SUBCASE("an m19.8a world is trivially valid and counts nothing") {
        std::string text = kGridLine;
        text += "tile\t0\t0\t1\t0\t1\t0\ta.rhf\n";
        auto w = TerrainWorld::parse(text);
        REQUIRE(w.has_value());
        CHECK(w->validate_levels());
        const TerrainWorldRefusals& r = w->refusals();
        CHECK(r.odd_lod_samples + r.missing_children + r.uncovered_tiles == 0);
    }
    SUBCASE("add_tile refuses a level beyond the cap, counted") {
        auto w = TerrainWorld::make(grid5());
        REQUIRE(w.has_value());
        TerrainWorldTile t{};
        t.level = rime::assets::kMaxTerrainLevel + 1;
        CHECK_FALSE(w->add_tile(t));
        CHECK(w->refusals().bad_levels == 1);
        CHECK(w->level_count() == 1);
    }
}

TEST_CASE("m19.8d1: floor-division parents — tiles -1 and 0 have different parents") {
    using rime::assets::terrain_parent_coord;
    CHECK(terrain_parent_coord({0, 1}) == TerrainTileCoord{0, 0});
    CHECK(terrain_parent_coord({-1, -2}) == TerrainTileCoord{-1, -1});
    CHECK(terrain_parent_coord({-3, 3}) == TerrainTileCoord{-2, 1});
}

TEST_CASE("m19.8d1: check_tile at a level — spacing 2^L bit for bit, one quantisation, dyadic "
          "placement with misalignment told apart") {
    const TerrainWorldGrid g = grid5();
    const TerrainTileKey k{1, {1, -1}};
    CHECK(check_tile(g, k, tile_at(g, k)) == TerrainTileCheck::Ok);
    CHECK(check_tile(g, TerrainTileKey{2, {0, 0}}, tile_at(g, {2, {0, 0}})) ==
          TerrainTileCheck::Ok);

    // Level-0 spacing on a level-1 key.
    HeightfieldAsset a = tile_at(g, k);
    a.cell_size_x = g.cell_size_x;
    CHECK(check_tile(g, k, a) == TerrainTileCheck::SpacingMismatch);
    // One ulp off the doubled spacing is still a mismatch: the comparison is bitwise.
    a = tile_at(g, k);
    a.cell_size_z = std::nextafter(a.cell_size_z, 10.0f);
    CHECK(check_tile(g, k, a) == TerrainTileCheck::SpacingMismatch);
    // A per-level quantisation is refused: every level shares the grid's.
    a = tile_at(g, k);
    a.height_scale *= 2.0f;
    CHECK(check_tile(g, k, a) == TerrainTileCheck::QuantisationMismatch);

    // Shifted by ONE level-0 tile: on the level-0 lattice, off the level-1 one — misaligned.
    a = tile_at(g, k);
    a.origin.x += g.pitch_x();
    CHECK(check_tile(g, k, a) == TerrainTileCheck::AlignmentMismatch);
    a = tile_at(g, k);
    a.origin.z -= g.pitch_z();
    CHECK(check_tile(g, k, a) == TerrainTileCheck::AlignmentMismatch);
    // Shifted by one level-1 tile: dyadically aligned, simply in the wrong place.
    a = tile_at(g, k);
    a.origin.x += g.pitch_x(1);
    CHECK(check_tile(g, k, a) == TerrainTileCheck::PlacementMismatch);
    // Off any lattice.
    a = tile_at(g, k);
    a.origin.x += 0.3f;
    CHECK(check_tile(g, k, a) == TerrainTileCheck::PlacementMismatch);
    // At level 0 a whole-tile shift is plain placement (there is no coarser lattice to miss).
    HeightfieldAsset l0 = tile_at(g, {0, {1, 1}});
    l0.origin.x += g.pitch_x();
    CHECK(check_tile(g, TerrainTileCoord{1, 1}, l0) == TerrainTileCheck::PlacementMismatch);

    TerrainWorldRefusals r{};
    r.count(TerrainTileCheck::AlignmentMismatch);
    CHECK(r.alignment_mismatches == 1);
    CHECK(r.placement_mismatches == 0);
}

TEST_CASE("m19.8d1: samples_coincide — every parent sample over a child equals the child's") {
    const TerrainWorldGrid g = grid5();
    const TerrainTileKey pk{1, {0, -1}};
    const HeightfieldAsset parent = tile_at(g, pk);
    std::vector<std::pair<TerrainTileKey, HeightfieldAsset>> kids;
    for (std::int32_t dz = 0; dz < 2; ++dz) {
        for (std::int32_t dx = 0; dx < 2; ++dx) {
            const TerrainTileKey ck{0, {dx, -2 + dz}};
            kids.emplace_back(ck, tile_at(g, ck));
        }
    }
    for (const auto& [ck, child] : kids) {
        CHECK(samples_coincide(parent, pk, child, ck));
    }
    // Doctor the centre sample, which all four children share at a corner: all four refuse.
    HeightfieldAsset doctored = parent;
    doctored.samples[doctored.index(2, 2)] ^= 1;
    for (const auto& [ck, child] : kids) {
        CHECK_FALSE(samples_coincide(doctored, pk, child, ck));
    }
    // Doctor (1, 1), inside the first quadrant only: only child (0, -2) refuses.
    doctored = parent;
    doctored.samples[doctored.index(1, 1)] ^= 1;
    CHECK_FALSE(samples_coincide(doctored, pk, kids[0].second, kids[0].first));
    for (std::size_t c = 1; c < kids.size(); ++c) {
        CHECK(samples_coincide(doctored, pk, kids[c].second, kids[c].first));
    }
    // Not this parent's child, or not one level down.
    CHECK_FALSE(samples_coincide(parent, pk, kids[0].second, TerrainTileKey{0, {2, -2}}));
    CHECK_FALSE(samples_coincide(parent, pk, kids[0].second, TerrainTileKey{1, {0, -2}}));
    // An even sample count cannot nest.
    HeightfieldAsset even = parent;
    even.columns = even.rows = 4;
    even.samples.resize(16);
    CHECK_FALSE(samples_coincide(even, pk, even, kids[0].first));
}

TEST_CASE("m19.8d1: verify_lod_chain accepts a clean chain and refuses a doctored one, counted") {
    MemoryWorld m = memory_world(4, 3); // 16 + 4 + 1 tiles
    const auto load = [&m](const TerrainWorldTile& t) { return m.load(t); };
    {
        TerrainWorldRefusals r{};
        const auto v = rime::assets::verify_lod_chain(m.world, load, r);
        CHECK(v.ok());
        CHECK(v.tiles_checked == 21);
        CHECK(v.pairs_checked == 4 * 5); // every parent against each of its four children
        CHECK(r.coincidence_mismatches == 0);
    }
    SUBCASE("a parent whose payload was swapped for different integers") {
        // The doctored manifest's header is perfect (spacing, quantisation, placement all fit);
        // only the shared samples betray it.
        HeightfieldAsset& p = m.files[key_path({1, {1, 0}})];
        p.samples[p.index(3, 4)] = static_cast<std::uint16_t>(p.samples[p.index(3, 4)] + 1);
        p.max_sample = std::max(p.max_sample, p.samples[p.index(3, 4)]);
        CHECK(check_tile(m.world.grid(), TerrainTileKey{1, {1, 0}}, p) == TerrainTileCheck::Ok);
        TerrainWorldRefusals r{};
        const auto v = rime::assets::verify_lod_chain(m.world, load, r);
        CHECK_FALSE(v.ok());
        CHECK(r.coincidence_mismatches == 1); // (3, 4) lies over one child only: (3, 1)
        CHECK(v.pairs_checked == 20);
    }
    SUBCASE("the root built from a block that started on the wrong child") {
        HeightfieldAsset& root = m.files[key_path({2, {0, 0}})];
        root.origin.x += m.world.grid().pitch_x();
        TerrainWorldRefusals r{};
        const auto v = rime::assets::verify_lod_chain(m.world, load, r);
        CHECK_FALSE(v.ok());
        CHECK(r.alignment_mismatches == 1);
        CHECK(v.pairs_checked == 16); // a misfit parent's pairs are not compared
    }
    SUBCASE("a missing file") {
        m.files.erase(key_path({0, {3, 3}}));
        TerrainWorldRefusals r{};
        const auto v = rime::assets::verify_lod_chain(m.world, load, r);
        CHECK_FALSE(v.ok());
        CHECK(r.unloadable_tiles == 2); // its own turn, and once as parent (1, 1)'s child
    }
}

TEST_CASE("m19.8d1: the Rust-cooked lod_world fixture verifies, and its geometric error and bounds "
          "recompute here from the cooked payloads") {
    const std::filesystem::path dir(RIME_ASSETS_FIXTURE_DIR);
    const auto text = rime::platform::read_file(dir / "lod_world.terrainworld");
    REQUIRE(text.has_value());
    const auto world = TerrainWorld::parse(
        std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
    REQUIRE(world.has_value());
    REQUIRE(world->level_count() == 2);
    REQUIRE(world->tiles().size() == 4);
    REQUIRE(world->tiles(1).size() == 1);

    const auto load = [&dir](const TerrainWorldTile& t) -> std::optional<HeightfieldAsset> {
        const auto bytes = rime::platform::read_file(dir / t.path);
        if (!bytes) {
            return std::nullopt;
        }
        rime::assets::AssetError err{};
        return rime::assets::read_heightfield(*bytes, err);
    };
    TerrainWorldRefusals r{};
    const auto v = rime::assets::verify_lod_chain(*world, load, r);
    CHECK(v.ok());
    CHECK(v.tiles_checked == 5);
    CHECK(v.pairs_checked == 4);

    // The parent's error, recomputed in C++ from the payloads: max over every level-0 sample of
    // |height − the parent's fixed-diagonal triangle at that point|, in metres, in double. The cook
    // rounds UP to f32, so the manifest value is the first float at or above this.
    const TerrainWorldTile& pt = world->tiles(1)[0];
    const auto parent = load(pt);
    REQUIRE(parent.has_value());
    const TerrainWorldGrid& g = world->grid();
    const std::uint32_t n = g.samples;
    const auto ph = [&](std::uint32_t i, std::uint32_t j) {
        return double{g.height_offset} +
               double{g.height_scale} * parent->samples[parent->index(i, j)];
    };
    double worst = 0.0;
    float lo = 1e30f;
    float hi = -1e30f;
    for (const TerrainWorldTile& t : world->tiles()) {
        const auto child = load(t);
        REQUIRE(child.has_value());
        for (std::uint32_t j = 0; j < n; ++j) {
            for (std::uint32_t i = 0; i < n; ++i) {
                // Position in parent cells: the child's quadrant offset plus half its sample index.
                const double px = t.coord.x * (n - 1) / 2.0 + i / 2.0;
                const double pz = t.coord.z * (n - 1) / 2.0 + j / 2.0;
                const auto ci = std::min(static_cast<std::uint32_t>(px), n - 2);
                const auto cj = std::min(static_cast<std::uint32_t>(pz), n - 2);
                const double u = px - ci;
                const double w = pz - cj;
                const double s = u >= w ? ph(ci, cj) + u * (ph(ci + 1, cj) - ph(ci, cj)) +
                                              w * (ph(ci + 1, cj + 1) - ph(ci + 1, cj))
                                        : ph(ci, cj) + w * (ph(ci, cj + 1) - ph(ci, cj)) +
                                              u * (ph(ci + 1, cj + 1) - ph(ci, cj + 1));
                const double truth = double{g.height_offset} +
                                     double{g.height_scale} * child->samples[child->index(i, j)];
                worst = std::max(worst, std::fabs(truth - s));
                const float y = g.origin.y + child->height(i, j);
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        }
    }
    CHECK(worst > 0.0);
    CHECK(double{pt.geometric_error} >= worst - 1e-9);
    CHECK(double{std::nextafter(pt.geometric_error, 0.0f)} <= worst + 1e-9);
    // Bounds: the parent's min/max are the extremes of every level-0 sample beneath it. Within a
    // few ulps rather than bitwise: Rust never fuses `offset + scale * q`, while Clang may contract
    // it into an FMA on targets that have one (arm64), which moves the last bit.
    CHECK(std::fabs(pt.min_y - lo) <= 1.0e-4f);
    CHECK(std::fabs(pt.max_y - hi) <= 1.0e-4f);
    for (const TerrainWorldTile& t : world->tiles()) {
        CHECK(t.geometric_error == 0.0f);
    }
}

TEST_CASE("m19.8d3: the Rust-cooked bake_world fixture — the 14-field parent line parses, and its "
          "two single-level bake textures read back with the ids the manifest records") {
    const std::filesystem::path dir(RIME_ASSETS_FIXTURE_DIR);
    const auto text = rime::platform::read_file(dir / "bake_world.terrainworld");
    REQUIRE(text.has_value());
    const auto world = TerrainWorld::parse(
        std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
    REQUIRE(world.has_value());
    REQUIRE(world->level_count() == 2);
    for (const TerrainWorldTile& t : world->tiles()) {
        CHECK_FALSE(t.has_bake()); // level 0 shades from its palette
    }
    REQUIRE(world->tiles(1).size() == 1);
    const TerrainWorldTile& parent = world->tiles(1)[0];
    REQUIRE(parent.has_bake());

    const auto load = [&dir](const std::string& path, rime::assets::AssetId* id) {
        const auto bytes = rime::platform::read_file(dir / path);
        REQUIRE(bytes.has_value());
        rime::assets::AssetError err{};
        auto tex = rime::assets::read_texture(*bytes, err, id);
        REQUIRE(tex.has_value());
        return *tex;
    };
    rime::assets::AssetId color_id{};
    rime::assets::AssetId material_id{};
    const rime::assets::TextureAsset color = load(parent.bake_color_path, &color_id);
    const rime::assets::TextureAsset material = load(parent.bake_material_path, &material_id);
    // The manifest's ids are the content hashes this reader computes: one cook, two languages.
    CHECK(color_id == parent.bake_color_id);
    CHECK(material_id == parent.bake_material_id);
    // ONE texel per sample, ONE level (a full chain for 5×5 would be 3), the two formats.
    const std::uint32_t n = world->grid().samples;
    for (const rime::assets::TextureAsset* t : {&color, &material}) {
        CHECK(t->width == n);
        CHECK(t->height == n);
        REQUIRE(t->mips.size() == 1);
        CHECK(t->mips[0].offset == 0);
        CHECK(t->pixels.size() == std::size_t{n} * n * 4);
    }
    CHECK(color.format == rime::assets::TextureFormat::Rgba8Srgb);
    CHECK(material.format == rime::assets::TextureFormat::Rgba8Unorm);

    // Two texels written down by hand (cook_fixture.rs builds the world for exactly this): the
    // parent's corner (0, 0) lies over a tile painted purely with layer A — linear (0.5, 0.25,
    // 1.0), metallic 0.75, roughness 0.125 — and corner (4, 0) over one painted purely with
    // layer B — (0.1, 0.8, 0.2), metallic 0, roughness 1. Colour is sRGB-encoded.
    const auto texel = [n](const rime::assets::TextureAsset& t, std::uint32_t i, std::uint32_t j) {
        std::array<int, 4> out{};
        for (std::size_t c = 0; c < 4; ++c) {
            out[c] = static_cast<int>(t.pixels[4 * (i + std::size_t{n} * j) + c]);
        }
        return out;
    };
    CHECK(texel(color, 0, 0) == std::array<int, 4>{188, 137, 255, 255});
    CHECK(texel(material, 0, 0) == std::array<int, 4>{191, 32, 0, 255});
    CHECK(texel(color, 4, 0) == std::array<int, 4>{89, 231, 124, 255});
    CHECK(texel(material, 4, 0) == std::array<int, 4>{0, 255, 0, 255});
    // Between them the edge fades from A to B, so the bake is not two flat halves.
    CHECK(texel(color, 2, 0) != texel(color, 0, 0));
    CHECK(texel(color, 2, 0) != texel(color, 4, 0));
}
