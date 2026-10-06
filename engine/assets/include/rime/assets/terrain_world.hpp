// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
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
//
// ── LEVELS (m19.8d1, ADR-0070) ──────────────────────────────────────────────────────────────────
//
// A world may also carry a LOD CHAIN: coarser tiles built by the cook (`rime terrain-world`) so a
// distant region can be drawn from one small tile instead of thousands of full-resolution ones.
// Their lines carry two more columns, a level first and a geometric error before the id:
//
//   tile <TAB> level <TAB> x <TAB> z <TAB> revision <TAB> min_y <TAB> max_y <TAB> geometric_error
//        <TAB> asset-id-hex <TAB> path
//
// The 8-field line above is exactly this with level 0 and error 0, so an m19.8a manifest reads
// unchanged. A level-L tile at (x, z) covers the 2^L x 2^L level-0 tiles from (x·2^L, z·2^L): it
// has the SAME `samples` as a level-0 tile and 2^L times the spacing. Its samples are EVERY SECOND
// sample of its four level-(L−1) children — not a filtered (averaged) copy — so every parent vertex
// IS a child vertex, bit for bit. That is what lets a renderer morph a child onto its parent and
// stitch two levels along an edge without a crack: the two surfaces share their coarse vertices
// exactly, so the only difference between them is the child's extra vertices, which a morph can
// slide onto the parent's triangles. A low-pass filter would give a smoother parent, but its
// vertices would sit at heights no child has, and every LOD edge would need a skirt or a seam fix.
//
// `min_y` / `max_y` of a parent bound EVERY level-0 sample beneath it, not just its own (a parent
// that skipped a peak must still be culled as tall as that peak). `geometric_error` is the largest
// vertical distance, in metres, between a level-0 sample beneath the tile and the tile's own
// triangulated surface (ADR-0060's fixed diagonal, barycentric) — what a renderer projects to the
// screen to decide whether the tile is detailed enough. Both come from the manifest, so a parent's
// usefulness is known before it is loaded and without loading anything beneath it.
//
// `validate_levels` (run by `parse`) checks the chain's SHAPE: every parent has all four children,
// every tile below the top level has a parent, and the grid's `samples - 1` is even. A chain that
// fails is dropped as a unit — level 0 still stands, the counters say why. The chain's CONTENT
// (spacing doubling, dyadic placement, coincident samples) needs the payloads and is checked when
// they are loaded: `check_tile` with a key, `samples_coincide`, and `verify_lod_chain`.
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

// The deepest LOD level a manifest may name. 2^16 level-0 tiles across one root is far beyond any
// world (a 65 km root over 1 m tiles); the cap keeps every `x << level` inside an i32 and every
// `2^level` exact in a float.
inline constexpr std::uint32_t kMaxTerrainLevel = 16;

// A tile at any level: level 0 is the cooked source tiles, level L covers 2^L x 2^L of them.
//
// Not an aggregate, on purpose: with aggregate initialisation `find({1, 0})` — m19.8a's spelling of
// "level-0 tile (1, 0)" — would also read as "level 1, coord {0}" and become ambiguous. The
// constructor demands a real TerrainTileCoord, so a braced pair still means a coordinate.
struct TerrainTileKey {
    std::uint32_t level = 0;
    TerrainTileCoord coord{};

    constexpr TerrainTileKey() noexcept = default;

    constexpr TerrainTileKey(std::uint32_t l, TerrainTileCoord c) noexcept : level(l), coord(c) {}

    friend constexpr bool operator==(TerrainTileKey a, TerrainTileKey b) noexcept {
        return a.level == b.level && a.coord == b.coord;
    }

    // Level first, then TerrainTileCoord's row-major order.
    friend constexpr bool operator<(TerrainTileKey a, TerrainTileKey b) noexcept {
        return a.level != b.level ? a.level < b.level : a.coord < b.coord;
    }
};

// The level-(L+1) tile containing level-L tile `c`: FLOOR division by 2, so tiles -1 and 0 have
// different parents (`-1 / 2 == 0` truncates and would put them under one). C++20 defines `>>` on a
// negative signed value as an arithmetic shift, which is exactly floor division by 2.
[[nodiscard]] constexpr TerrainTileCoord terrain_parent_coord(TerrainTileCoord c) noexcept {
    return {c.x >> 1, c.z >> 1};
}

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

    // A level's spacing and pitch: 2^level times level 0's. Scaling a float by a power of two is
    // exact (only the exponent changes), so these are bit-identical to what the cook writes.
    [[nodiscard]] float cell_size_x_at(std::uint32_t level) const noexcept {
        return cell_size_x * level_scale(level);
    }

    [[nodiscard]] float cell_size_z_at(std::uint32_t level) const noexcept {
        return cell_size_z * level_scale(level);
    }

    [[nodiscard]] float pitch_x(std::uint32_t level) const noexcept {
        return pitch_x() * level_scale(level);
    }

    [[nodiscard]] float pitch_z(std::uint32_t level) const noexcept {
        return pitch_z() * level_scale(level);
    }

    [[nodiscard]] static float level_scale(std::uint32_t level) noexcept {
        return static_cast<float>(std::uint32_t{1} << (level & 31U));
    }

    // Where tile `c`'s heightfield must have its `origin`.
    [[nodiscard]] core::Vec3 tile_origin(TerrainTileCoord c) const noexcept {
        return {origin.x + static_cast<float>(c.x) * pitch_x(),
                origin.y,
                origin.z + static_cast<float>(c.z) * pitch_z()};
    }

    // Where a tile of any level must have its `origin` (level 0: the overload above).
    [[nodiscard]] core::Vec3 tile_origin(TerrainTileKey k) const noexcept {
        return {origin.x + static_cast<float>(k.coord.x) * pitch_x(k.level),
                origin.y,
                origin.z + static_cast<float>(k.coord.z) * pitch_z(k.level)};
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
    // m19.8d1 (after `path`, so m19.8a's aggregate initialisers still mean what they meant).
    std::uint32_t level = 0;      // LOD level; 0 = a cooked source tile
    float geometric_error = 0.0f; // metres; 0 for level 0 (see the header comment)

    [[nodiscard]] TerrainTileKey key() const noexcept { return {level, coord}; }
};

// Why a tile was turned away. Counted per reason in `TerrainWorldRefusals`.
enum class TerrainTileCheck : std::uint8_t {
    Ok,
    SizeMismatch,         // columns / rows differ from the grid's `samples`
    SpacingMismatch,      // cell_size_x / cell_size_z differ from the grid's, bit for bit
    QuantisationMismatch, // height_scale / height_offset differ from the grid's, bit for bit
    PlacementMismatch,    // origin is not where the grid puts this coordinate (± 1 mm)
    // m19.8d1: a parent whose origin sits on a level-0 tile corner that is not a multiple of 2^L
    // tiles — built from a block that started on the wrong child, so its children are not the four
    // tiles the key names.
    AlignmentMismatch,
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
    // m19.8d1 — the LOD chain. The first five are the chain's shape and content; `bad_levels` is an
    // add_tile beyond kMaxTerrainLevel; `unloadable_tiles` a tile verify_lod_chain could not load.
    std::uint64_t alignment_mismatches = 0;   // check_tile: parent off its dyadic position
    std::uint64_t odd_lod_samples = 0;        // a chain on a grid whose samples - 1 is odd
    std::uint64_t missing_children = 0;       // parents lacking one or more of their four children
    std::uint64_t uncovered_tiles = 0;        // tiles below the top level with no parent
    std::uint64_t coincidence_mismatches = 0; // parent/child pairs whose shared samples differ
    std::uint64_t bad_levels = 0;
    std::uint64_t unloadable_tiles = 0;

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

    // Add a tile at its `level`. Refuses (returns false) a key already present (counts
    // `duplicate_coords`) or a level beyond kMaxTerrainLevel (counts `bad_levels`). Adding parents
    // by hand leaves the chain unchecked until `validate_levels` runs.
    bool add_tile(TerrainWorldTile tile);

    // Check the LOD chain's shape: (samples - 1) even, every parent's four children present, every
    // tile below the top level parented. On any failure the whole chain is dropped (level 0 kept),
    // each cause counted, and false returned. A world with only level 0 is trivially valid.
    bool validate_levels();

    [[nodiscard]] const TerrainWorldGrid& grid() const noexcept { return grid_; }

    // Level 0 — the m19.8a view, which the streamer walks.
    [[nodiscard]] const std::vector<TerrainWorldTile>& tiles() const noexcept { return levels_[0]; }

    // One level's tiles in TerrainTileCoord order; empty past the top.
    [[nodiscard]] const std::vector<TerrainWorldTile>& tiles(std::uint32_t level) const noexcept;

    // 1 + the highest level holding a tile (1 for an m19.8a world).
    [[nodiscard]] std::uint32_t level_count() const noexcept;

    [[nodiscard]] const TerrainWorldTile* find(TerrainTileCoord c) const noexcept;

    [[nodiscard]] const TerrainWorldTile* find(TerrainTileKey k) const noexcept;

    // The coordinates whose footprint lies within `radius` of `p` in XZ, in TerrainTileCoord order.
    // Walks only the square of grid cells the radius can reach, not every tile.
    [[nodiscard]] std::vector<TerrainTileCoord> tiles_within(core::Vec3 p, float radius) const;

    [[nodiscard]] const TerrainWorldRefusals& refusals() const noexcept { return refusals_; }

    [[nodiscard]] TerrainWorldRefusals& refusals() noexcept { return refusals_; }

private:
    TerrainWorldGrid grid_{};
    // levels_[L] holds level L's tiles sorted by coord; levels_[0] always exists.
    std::vector<std::vector<TerrainWorldTile>> levels_ =
        std::vector<std::vector<TerrainWorldTile>>(1);
    TerrainWorldRefusals refusals_{};
};

// Does `asset` fit the grid at coordinate `c`? Spacing and quantisation are compared BIT FOR BIT —
// equal integers must mean equal heights — and placement within 1 mm.
[[nodiscard]] TerrainTileCheck
check_tile(const TerrainWorldGrid& grid, TerrainTileCoord c, const HeightfieldAsset& asset);

// The same at any level: the spacing must be exactly 2^L times the grid's, the quantisation the
// grid's (ONE quantisation for every level, so a parent's integer and a child's integer at the same
// spot are comparable directly), the origin the key's dyadic position.
[[nodiscard]] TerrainTileCheck
check_tile(const TerrainWorldGrid& grid, TerrainTileKey k, const HeightfieldAsset& asset);

// The nesting invariant: every sample of `parent` that lies over `child` equals the child's sample
// at that spot. `child_key` must be one of `parent_key`'s four children and both assets square with
// the same odd sample count; anything else is false. Equal integers are equal heights only under
// one quantisation, so run check_tile on both first.
[[nodiscard]] bool samples_coincide(const HeightfieldAsset& parent,
                                    TerrainTileKey parent_key,
                                    const HeightfieldAsset& child,
                                    TerrainTileKey child_key) noexcept;

// What verify_lod_chain looked at. Guardrail 5: a verifier that cannot see what it skipped still
// reads as passing, so it reports how much it checked — `pairs_checked` must be 4 x the parents.
struct TerrainLodVerification {
    std::uint64_t tiles_checked = 0; // tiles loaded and run through check_tile
    std::uint64_t pairs_checked = 0; // parent/child pairs compared sample by sample
    std::uint64_t failures = 0;      // tiles or pairs refused (by cause in the refusals)

    [[nodiscard]] bool ok() const noexcept { return failures == 0; }
};

// Load every tile of every level through `load` (nullopt = could not load: `unloadable_tiles`),
// check_tile each against its key, and check samples_coincide for every parent with each of its
// four children. Every refusal is counted into `refusals`. The loader is a callback so the check
// owns no IO policy: a tool reads files, a test hands in doctored assets, the streamer (m19.8d2)
// runs the same two checks per pair as payloads arrive.
[[nodiscard]] TerrainLodVerification verify_lod_chain(
    const TerrainWorld& world,
    const std::function<std::optional<HeightfieldAsset>(const TerrainWorldTile&)>& load,
    TerrainWorldRefusals& refusals);

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
