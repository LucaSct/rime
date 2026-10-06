// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rime/assets/asset_id.hpp"
#include "rime/core/math/vec.hpp"

// The TERRAIN WORLD MANIFEST (m19.8a, ADR-0069): which heightfield tile sits at which square of an
// integer grid.
//
// ADR-0060 §2's answer to "how big can terrain be" was MANY TILES, not one enormous one, and m19.8b
// (ADR-0067) gave the asset server a way to give a tile back. What was missing is the map: a list
// that says tile (3, -2) is `hills_3_-2.rhf`, so a streamer can ask "which tiles are near the
// camera?" without opening every file to read its origin. This is that list.
//
// ── WHY THE GRID IS DECLARED ONCE, NOT PER TILE ─────────────────────────────────────────────────
//
// Adjacent tiles must meet without a crack. On the GPU a tile's vertex (i, j) sits at
// `origin + (i * cell_x, offset + scale * q, j * cell_z)` (terrain.vert, ADR-0062). Two tiles share
// an edge exactly when (a) they use the SAME spacing, so their edge vertices land on the same XZ
// positions, (b) the SAME quantisation (scale and offset), so equal integers mean equal heights,
// and (c) the SAME integers along the shared edge. (a) and (b) are properties of the whole world,
// so the manifest states them once in its `grid` line, and every tile is checked against that one
// statement as it loads (`check_tile`). (c) is a property of a PAIR of tiles and is checked between
// neighbours (`edges_match`). Each check REFUSES rather than repairs, the posture ADR-0060 §2 set
// for registration: a tile that would crack the ground is not drawn, and the refusal is counted.
//
// Comparing the integers — not dequantised floats with a tolerance — is what (b) buys: with one
// shared quantisation, two equal u16s ARE two equal heights, bit for bit, on every GPU.
//
// ── THE FILE ────────────────────────────────────────────────────────────────────────────────────
//
// Tab-separated lines, the house style of `Manifest` (manifest.hpp) so a path may hold spaces:
//
//   # comment
//   grid <TAB> samples <TAB> cell_x <TAB> cell_z <TAB> height_scale <TAB> height_offset
//        <TAB> origin_x <TAB> origin_y <TAB> origin_z
//   tile <TAB> x <TAB> z <TAB> revision <TAB> min_y <TAB> max_y <TAB> asset-id-hex <TAB> path
//
// Exactly one `grid` line, before any `tile` line. `samples` is the per-axis sample count of EVERY
// tile (square tiles; ADR-0060's grid permits rectangles, a world does not need them), so a tile
// spans `cell * (samples - 1)` metres and tile (x, z)'s local origin is
// `origin + (x * pitch_x, 0, z * pitch_z)`. `path` is relative to the manifest's directory unless
// absolute. `asset-id-hex` may be 0 when the writer does not know it. `min_y` / `max_y` are the
// tile's world-space height bounds (for culling; nothing in m19.8a reads them yet). `revision` is
// the tile's content revision, carried for brick 8c, which replicates it.
//
// A malformed line makes `parse` return nullopt (a machine-written file with a bad line is a bug,
// not something to paper over — Manifest's rule). A DUPLICATE coordinate is refused and counted
// instead: the first tile at a coordinate wins, the rest are reported in `refusals()`.
namespace rime::assets {

struct HeightfieldAsset;

// A tile's integer grid coordinate: tile (x, z) covers world X in [x·pitch_x, (x+1)·pitch_x) and
// Z likewise, relative to the grid origin.
struct TerrainTileCoord {
    std::int32_t x = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(TerrainTileCoord a, TerrainTileCoord b) noexcept {
        return a.x == b.x && a.z == b.z;
    }

    // Row-major (z, then x) — a stable order, so anything iterating tiles in this order is
    // deterministic across runs and machines.
    friend constexpr bool operator<(TerrainTileCoord a, TerrainTileCoord b) noexcept {
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    }
};

// The world-wide grid every tile must agree with.
struct TerrainWorldGrid {
    std::uint32_t samples = 0; // per axis, every tile (>= 2)
    float cell_size_x = 0.0f;  // metres between samples along X (> 0)
    float cell_size_z = 0.0f;  // metres between samples along Z (> 0)
    float height_scale = 0.0f; // metres per quantisation step (> 0), shared by every tile
    float height_offset = 0.0f;
    core::Vec3 origin{0.0f, 0.0f, 0.0f}; // world position of tile (0, 0)'s local origin

    [[nodiscard]] float pitch_x() const noexcept {
        return cell_size_x * static_cast<float>(samples - 1);
    }

    [[nodiscard]] float pitch_z() const noexcept {
        return cell_size_z * static_cast<float>(samples - 1);
    }

    // Where tile `c`'s heightfield must have its `origin`.
    [[nodiscard]] core::Vec3 tile_origin(TerrainTileCoord c) const noexcept {
        return {origin.x + static_cast<float>(c.x) * pitch_x(),
                origin.y,
                origin.z + static_cast<float>(c.z) * pitch_z()};
    }

    // The XZ distance from `p` to tile `c`'s footprint: 0 inside it. The streamer's radii are
    // measured with this, so a camera standing on a tile always wants that tile whatever the radii.
    [[nodiscard]] float distance_xz(TerrainTileCoord c, core::Vec3 p) const noexcept;
};

struct TerrainWorldTile {
    TerrainTileCoord coord{};
    std::uint32_t revision = 0;
    float min_y = 0.0f;
    float max_y = 0.0f;
    AssetId id{};     // 0 = not recorded
    std::string path; // as written; resolve against the manifest's directory
};

// Why a tile was turned away. Counted per reason in `TerrainWorldRefusals`.
enum class TerrainTileCheck : std::uint8_t {
    Ok,
    SizeMismatch,         // columns / rows differ from the grid's `samples`
    SpacingMismatch,      // cell_size_x / cell_size_z differ from the grid's, bit for bit
    QuantisationMismatch, // height_scale / height_offset differ from the grid's, bit for bit
    PlacementMismatch,    // origin is not where the grid puts this coordinate (± 1 mm)
};

// Guardrail 5: a refused tile and a tile nobody listed both draw as a hole, so every refusal has a
// counter. `duplicate_coords` is moved by `parse` / `add_tile`; the rest by whoever checks a loaded
// tile (TerrainResidency) through `count`.
struct TerrainWorldRefusals {
    std::uint64_t duplicate_coords = 0;
    std::uint64_t size_mismatches = 0;
    std::uint64_t spacing_mismatches = 0;
    std::uint64_t quantisation_mismatches = 0;
    std::uint64_t placement_mismatches = 0;
    std::uint64_t border_mismatches = 0;

    void count(TerrainTileCheck check) noexcept;
};

class TerrainWorld {
public:
    // An empty world on `grid`. Returns nullopt for a grid no tile could satisfy (samples < 2, or a
    // non-finite / non-positive spacing or scale, or a non-finite offset or origin).
    [[nodiscard]] static std::optional<TerrainWorld> make(const TerrainWorldGrid& grid);

    // Parse the file format above. nullopt (logged with the line number) on a malformed line, a
    // missing or repeated `grid` line, or a `tile` line before the grid.
    [[nodiscard]] static std::optional<TerrainWorld> parse(std::string_view text);

    // Add a tile. Refuses (returns false, counts `duplicate_coords`) a coordinate already present.
    bool add_tile(TerrainWorldTile tile);

    [[nodiscard]] const TerrainWorldGrid& grid() const noexcept { return grid_; }

    [[nodiscard]] const std::vector<TerrainWorldTile>& tiles() const noexcept { return tiles_; }

    [[nodiscard]] const TerrainWorldTile* find(TerrainTileCoord c) const noexcept;

    // The coordinates whose footprint lies within `radius` of `p` in XZ, in TerrainTileCoord order.
    // Walks only the square of grid cells the radius can reach, not every tile.
    [[nodiscard]] std::vector<TerrainTileCoord> tiles_within(core::Vec3 p, float radius) const;

    [[nodiscard]] const TerrainWorldRefusals& refusals() const noexcept { return refusals_; }

    [[nodiscard]] TerrainWorldRefusals& refusals() noexcept { return refusals_; }

private:
    TerrainWorldGrid grid_{};
    std::vector<TerrainWorldTile> tiles_; // sorted by coord
    TerrainWorldRefusals refusals_{};
};

// Does `asset` fit the grid at coordinate `c`? Spacing and quantisation are compared BIT FOR BIT —
// equal integers must mean equal heights — and placement within 1 mm.
[[nodiscard]] TerrainTileCheck
check_tile(const TerrainWorldGrid& grid, TerrainTileCoord c, const HeightfieldAsset& asset);

// A tile's four border sample rows, kept so a neighbour that arrives later can be checked against
// a tile whose CPU payload has already been released. South is j = 0 (−Z), north j = rows − 1,
// west i = 0 (−X), east i = columns − 1.
struct TerrainTileEdges {
    std::vector<std::uint16_t> south;
    std::vector<std::uint16_t> north;
    std::vector<std::uint16_t> west;
    std::vector<std::uint16_t> east;
};

[[nodiscard]] TerrainTileEdges tile_edges(const HeightfieldAsset& asset);

// True when the two tiles are not edge-adjacent, or when the samples along their shared edge are
// the same integers. False is a crack waiting to be drawn.
[[nodiscard]] bool edges_match(const TerrainTileEdges& a,
                               TerrainTileCoord a_coord,
                               const TerrainTileEdges& b,
                               TerrainTileCoord b_coord) noexcept;

} // namespace rime::assets
