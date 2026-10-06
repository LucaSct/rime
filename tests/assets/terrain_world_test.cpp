// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m19.8a — the terrain world manifest (ADR-0069 §1). CPU-only: parsing, the duplicate-coordinate
// refusal, check_tile's four refusals, the border comparison, and the radius query the streamer
// is built on. The residency's use of all of it is proven in tests/terrain_render.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "rime/assets/heightfield_asset.hpp"
#include "rime/assets/terrain_world.hpp"

namespace {

using rime::assets::check_tile;
using rime::assets::edges_match;
using rime::assets::HeightfieldAsset;
using rime::assets::TerrainTileCheck;
using rime::assets::TerrainTileCoord;
using rime::assets::TerrainWorld;
using rime::assets::TerrainWorldGrid;

TerrainWorldGrid grid9() {
    TerrainWorldGrid g{};
    g.samples = 9;
    g.cell_size_x = 1.0f;
    g.cell_size_z = 1.0f;
    g.height_scale = 0.01f;
    g.height_offset = -2.0f;
    g.origin = {100.0f, 5.0f, -40.0f};
    return g;
}

// A tile of the GLOBAL integer field q(gx, gz) — so any two tiles cut from it agree on their
// shared edges by construction.
std::uint16_t field(std::int32_t gx, std::int32_t gz) {
    return static_cast<std::uint16_t>(3000 + 37 * gx + 53 * gz + ((gx * gz) & 15));
}

HeightfieldAsset tile_at(const TerrainWorldGrid& g, TerrainTileCoord c) {
    HeightfieldAsset a{};
    a.columns = g.samples;
    a.rows = g.samples;
    a.cell_size_x = g.cell_size_x;
    a.cell_size_z = g.cell_size_z;
    a.height_scale = g.height_scale;
    a.height_offset = g.height_offset;
    a.origin = g.tile_origin(c);
    const std::int32_t n = static_cast<std::int32_t>(g.samples) - 1;
    for (std::uint32_t j = 0; j < g.samples; ++j) {
        for (std::uint32_t i = 0; i < g.samples; ++i) {
            a.samples.push_back(field(c.x * n + static_cast<std::int32_t>(i),
                                      c.z * n + static_cast<std::int32_t>(j)));
        }
    }
    return a;
}

} // namespace

TEST_CASE("m19.8a: a terrain world file parses — grid, tiles, ids, paths with spaces") {
    const std::string text = "# rime-terrain-world v1\n"
                             "grid\t9\t1\t1\t0.01\t-2\t100\t5\t-40\n"
                             "tile\t0\t0\t3\t1.5\t9.25\t00000000000000ab\thills/tile 0 0.rhf\n"
                             "tile\t-1\t2\t0\t0\t1\t0\t/abs/t.rhf\r\n";
    const auto w = TerrainWorld::parse(text);
    REQUIRE(w.has_value());
    CHECK(w->grid().samples == 9);
    CHECK(w->grid().height_offset == -2.0f);
    CHECK(w->grid().origin.z == -40.0f);
    REQUIRE(w->tiles().size() == 2);
    // Sorted by coordinate (z, then x): (0,0) before (-1,2).
    CHECK(w->tiles()[0].coord == TerrainTileCoord{0, 0});
    CHECK(w->tiles()[0].revision == 3);
    CHECK(w->tiles()[0].id.value == 0xab);
    CHECK(w->tiles()[0].path == "hills/tile 0 0.rhf");
    CHECK(w->tiles()[0].max_y == 9.25f);
    const auto* t = w->find({-1, 2});
    REQUIRE(t != nullptr);
    CHECK(t->path == "/abs/t.rhf");
    CHECK(w->find({5, 5}) == nullptr);
    CHECK(w->refusals().duplicate_coords == 0);
}

TEST_CASE("m19.8a: malformed terrain world files are refused whole") {
    CHECK_FALSE(TerrainWorld::parse("tile\t0\t0\t0\t0\t1\t0\ta.rhf\n").has_value()); // no grid
    CHECK_FALSE(TerrainWorld::parse("grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n"
                                    "grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n")
                    .has_value()); // repeated grid
    CHECK_FALSE(TerrainWorld::parse("grid\t1\t1\t1\t0.01\t0\t0\t0\t0\n").has_value()); // samples
    CHECK_FALSE(TerrainWorld::parse("grid\t9\t0\t1\t0.01\t0\t0\t0\t0\n").has_value()); // spacing
    CHECK_FALSE(TerrainWorld::parse("grid\t9\t1\t1\tnan\t0\t0\t0\t0\n").has_value());  // scale
    CHECK_FALSE(TerrainWorld::parse("grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n"
                                    "tile\t0\t0\t0\t0\t1\tzz\ta.rhf\n")
                    .has_value()); // bad id
    CHECK_FALSE(TerrainWorld::parse("grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n"
                                    "tile\t0\t0\t0\t0\t1\t0\t\n")
                    .has_value()); // empty path
    CHECK_FALSE(TerrainWorld::parse("grid\t9\t1\t1\t0.01\t0\t0\t0\t0\nmystery\n").has_value());
    CHECK_FALSE(TerrainWorld::parse("").has_value());
}

TEST_CASE("m19.8a: a duplicate coordinate is refused and counted — the first tile wins") {
    const auto w = TerrainWorld::parse("grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n"
                                       "tile\t1\t1\t0\t0\t1\t0\tfirst.rhf\n"
                                       "tile\t1\t1\t7\t0\t1\t0\tsecond.rhf\n"
                                       "tile\t2\t1\t0\t0\t1\t0\tother.rhf\n");
    REQUIRE(w.has_value());
    CHECK(w->tiles().size() == 2);
    CHECK(w->refusals().duplicate_coords == 1);
    CHECK(w->find({1, 1})->path == "first.rhf");

    auto made = TerrainWorld::make(grid9());
    REQUIRE(made.has_value());
    CHECK(made->add_tile({{0, 0}, 0, 0.0f, 1.0f, {}, "a"}));
    CHECK_FALSE(made->add_tile({{0, 0}, 1, 0.0f, 1.0f, {}, "b"}));
    CHECK(made->refusals().duplicate_coords == 1);
}

TEST_CASE("m19.8a: check_tile refuses size, spacing, quantisation and placement mismatches") {
    const TerrainWorldGrid g = grid9();
    const TerrainTileCoord c{2, -1};
    CHECK(check_tile(g, c, tile_at(g, c)) == TerrainTileCheck::Ok);

    HeightfieldAsset a = tile_at(g, c);
    a.cell_size_x = 1.0000001f; // one ULP-ish off: bit equality is the rule
    CHECK(check_tile(g, c, a) == TerrainTileCheck::SpacingMismatch);
    a = tile_at(g, c);
    a.cell_size_z = 2.0f;
    CHECK(check_tile(g, c, a) == TerrainTileCheck::SpacingMismatch);
    a = tile_at(g, c);
    a.height_scale = 0.02f;
    CHECK(check_tile(g, c, a) == TerrainTileCheck::QuantisationMismatch);
    a = tile_at(g, c);
    a.height_offset = 0.0f;
    CHECK(check_tile(g, c, a) == TerrainTileCheck::QuantisationMismatch);
    a = tile_at(g, c);
    a.origin.x += 0.01f; // 1 cm off its cell
    CHECK(check_tile(g, c, a) == TerrainTileCheck::PlacementMismatch);
    a = tile_at(g, c);
    a.origin.x += 0.0005f; // half a millimetre: inside the tolerance
    CHECK(check_tile(g, c, a) == TerrainTileCheck::Ok);
    CHECK(check_tile(g, {3, -1}, tile_at(g, c)) == TerrainTileCheck::PlacementMismatch);
    a = tile_at(g, c);
    a.columns = 8;
    CHECK(check_tile(g, c, a) == TerrainTileCheck::SizeMismatch);

    rime::assets::TerrainWorldRefusals r{};
    r.count(TerrainTileCheck::SpacingMismatch);
    r.count(TerrainTileCheck::QuantisationMismatch);
    r.count(TerrainTileCheck::PlacementMismatch);
    r.count(TerrainTileCheck::SizeMismatch);
    r.count(TerrainTileCheck::Ok);
    CHECK(r.spacing_mismatches == 1);
    CHECK(r.quantisation_mismatches == 1);
    CHECK(r.placement_mismatches == 1);
    CHECK(r.size_mismatches == 1);
}

TEST_CASE("m19.8a: edges_match compares the shared border integers of edge neighbours only") {
    const TerrainWorldGrid g = grid9();
    const TerrainTileCoord c{0, 0};
    const auto e = rime::assets::tile_edges(tile_at(g, c));
    for (const TerrainTileCoord n : {TerrainTileCoord{1, 0},
                                     TerrainTileCoord{-1, 0},
                                     TerrainTileCoord{0, 1},
                                     TerrainTileCoord{0, -1}}) {
        const auto en = rime::assets::tile_edges(tile_at(g, n));
        CHECK(edges_match(e, c, en, n));
        CHECK(edges_match(en, n, e, c));
    }
    // One sample on tile (1,0)'s WEST edge moved by one quantisation step: a crack.
    HeightfieldAsset east = tile_at(g, {1, 0});
    east.samples[east.index(0, 4)] += 1;
    const auto bad = rime::assets::tile_edges(east);
    CHECK_FALSE(edges_match(e, c, bad, {1, 0}));
    CHECK_FALSE(edges_match(bad, {1, 0}, e, c));
    // An interior change is not a border mismatch, and a non-adjacent pair shares nothing.
    HeightfieldAsset inner = tile_at(g, {1, 0});
    inner.samples[inner.index(4, 4)] += 9;
    CHECK(edges_match(e, c, rime::assets::tile_edges(inner), {1, 0}));
    CHECK(edges_match(e, c, bad, {1, 1}));
    CHECK(edges_match(e, c, bad, {3, 0}));
}

TEST_CASE("m19.8a: tiles_within measures from the tile footprint, so the camera's own tile is "
          "always wanted") {
    TerrainWorldGrid g = grid9();
    g.origin = {0.0f, 0.0f, 0.0f};
    auto w = TerrainWorld::make(g);
    REQUIRE(w.has_value());
    for (std::int32_t z = 0; z < 4; ++z) {
        for (std::int32_t x = 0; x < 4; ++x) {
            REQUIRE(w->add_tile({{x, z}, 0, 0.0f, 1.0f, {}, "t"}));
        }
    }
    // Pitch 8 m. A point in the middle of tile (1,1) with radius 0 wants exactly that tile.
    auto in = w->tiles_within({12.0f, 0.0f, 12.0f}, 0.0f);
    REQUIRE(in.size() == 1);
    CHECK(in[0] == TerrainTileCoord{1, 1});
    // Radius 4.5 from the centre of (1,1) reaches its four edge neighbours, not the diagonals
    // (corner distance 4·√2 ≈ 5.66).
    CHECK(w->tiles_within({12.0f, 0.0f, 12.0f}, 4.5f).size() == 5);
    CHECK(w->tiles_within({12.0f, 0.0f, 12.0f}, 5.7f).size() == 9);
    // Off the world's edge: only listed tiles come back.
    CHECK(w->tiles_within({-100.0f, 0.0f, -100.0f}, 10.0f).empty());
    CHECK(w->tiles_within({0.0f, 0.0f, 0.0f}, 1.0e30f).size() == 16);
    CHECK(g.distance_xz({1, 1}, {12.0f, 0.0f, 12.0f}) == 0.0f);
    CHECK(g.distance_xz({2, 1}, {12.0f, 0.0f, 12.0f}) == 4.0f);
}
