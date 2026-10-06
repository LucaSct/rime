// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/assets/terrain_world.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "rime/assets/heightfield_asset.hpp"
#include "rime/core/diagnostics/log.hpp"

namespace rime::assets {
namespace {

// How far a tile's cooked origin may sit from where the grid puts it. The origin is an authored
// f32 and the grid's position is computed (origin + x * pitch), so demanding bit equality would
// refuse a correct world over one rounding step; a millimetre is far below a sample spacing and far
// above any rounding a sane world coordinate produces.
constexpr float kPlacementTolerance = 1.0e-3f;

// Split a line into exactly `N` tab-separated fields (Manifest's rule: the wrong count is
// malformed, never padded or truncated).
template <std::size_t N>
[[nodiscard]] bool split_tabs(std::string_view line,
                              std::array<std::string_view, N>& out) noexcept {
    std::size_t field = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        const std::string_view piece = line.substr(
            start, tab == std::string_view::npos ? std::string_view::npos : tab - start);
        if (field >= N) {
            return false;
        }
        out[field++] = piece;
        if (tab == std::string_view::npos) {
            break;
        }
        start = tab + 1;
    }
    return field == N;
}

template <class T> [[nodiscard]] bool parse_int(std::string_view tok, T& out, int base = 10) {
    if (tok.empty()) {
        return false;
    }
    const char* const last = tok.data() + tok.size();
    const auto [ptr, ec] = std::from_chars(tok.data(), last, out, base);
    return ec == std::errc{} && ptr == last;
}

// Floats through strtof, not from_chars: libc++ on the macOS SDKs we target deletes the float
// overload (scene_format.cpp has the full story). The token is copied so strtof sees a bounded,
// NUL-terminated string, and the end pointer proves the whole token was the number.
[[nodiscard]] bool parse_float(std::string_view tok, float& out) {
    if (tok.empty()) {
        return false;
    }
    const std::string buf(tok);
    char* end = nullptr;
    errno = 0;
    out = std::strtof(buf.c_str(), &end);
    return end == buf.c_str() + buf.size() && std::isfinite(out);
}

[[nodiscard]] bool grid_ok(const TerrainWorldGrid& g) noexcept {
    const auto pos = [](float v) { return std::isfinite(v) && v > 0.0f; };
    return g.samples >= 2 && pos(g.cell_size_x) && pos(g.cell_size_z) && pos(g.height_scale) &&
           std::isfinite(g.height_offset) && std::isfinite(g.origin.x) &&
           std::isfinite(g.origin.y) && std::isfinite(g.origin.z);
}

} // namespace

void TerrainWorldRefusals::count(TerrainTileCheck check) noexcept {
    switch (check) {
        case TerrainTileCheck::Ok:
            return;
        case TerrainTileCheck::SizeMismatch:
            ++size_mismatches;
            return;
        case TerrainTileCheck::SpacingMismatch:
            ++spacing_mismatches;
            return;
        case TerrainTileCheck::QuantisationMismatch:
            ++quantisation_mismatches;
            return;
        case TerrainTileCheck::PlacementMismatch:
            ++placement_mismatches;
            return;
    }
}

float TerrainWorldGrid::distance_xz(TerrainTileCoord c, core::Vec3 p) const noexcept {
    const core::Vec3 o = tile_origin(c);
    // Distance from a point to an axis-aligned rectangle: clamp the point into it, measure the
    // offset. Zero inside, the edge distance beside it, the corner distance diagonally off it.
    const float dx = std::max({o.x - p.x, 0.0f, p.x - (o.x + pitch_x())});
    const float dz = std::max({o.z - p.z, 0.0f, p.z - (o.z + pitch_z())});
    return std::sqrt(dx * dx + dz * dz);
}

std::optional<TerrainWorld> TerrainWorld::make(const TerrainWorldGrid& grid) {
    if (!grid_ok(grid)) {
        return std::nullopt;
    }
    TerrainWorld w;
    w.grid_ = grid;
    return w;
}

std::optional<TerrainWorld> TerrainWorld::parse(std::string_view text) {
    std::optional<TerrainWorld> world;
    std::size_t line_start = 0;
    int line_number = 0;
    while (line_start <= text.size()) {
        const std::size_t newline = text.find('\n', line_start);
        const std::size_t line_end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(line_start, line_end - line_start);
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty() && line.front() != '#') {
            if (line.starts_with("grid\t")) {
                std::array<std::string_view, 9> f{};
                TerrainWorldGrid g{};
                const bool ok =
                    !world && split_tabs(line, f) && parse_int(f[1], g.samples) &&
                    parse_float(f[2], g.cell_size_x) && parse_float(f[3], g.cell_size_z) &&
                    parse_float(f[4], g.height_scale) && parse_float(f[5], g.height_offset) &&
                    parse_float(f[6], g.origin.x) && parse_float(f[7], g.origin.y) &&
                    parse_float(f[8], g.origin.z);
                if (ok) {
                    world = make(g);
                }
                if (!ok || !world) {
                    RIME_ERROR("terrain world: line {}: bad or repeated grid line", line_number);
                    return std::nullopt;
                }
            } else if (line.starts_with("tile\t")) {
                std::array<std::string_view, 8> f{};
                TerrainWorldTile t{};
                std::uint64_t id = 0;
                const bool ok = world && split_tabs(line, f) && parse_int(f[1], t.coord.x) &&
                                parse_int(f[2], t.coord.z) && parse_int(f[3], t.revision) &&
                                parse_float(f[4], t.min_y) && parse_float(f[5], t.max_y) &&
                                parse_int(f[6], id, 16) && !f[7].empty();
                if (!ok) {
                    RIME_ERROR("terrain world: line {}: malformed tile line (or no grid line "
                               "before it)",
                               line_number);
                    return std::nullopt;
                }
                t.id = AssetId{id};
                t.path = std::string(f[7]);
                if (!world->add_tile(std::move(t))) {
                    RIME_WARN("terrain world: line {}: duplicate coordinate, refused", line_number);
                }
            } else {
                RIME_ERROR("terrain world: line {}: unknown record", line_number);
                return std::nullopt;
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }
    if (!world) {
        RIME_ERROR("terrain world: no grid line");
    }
    return world;
}

bool TerrainWorld::add_tile(TerrainWorldTile tile) {
    const auto it =
        std::lower_bound(tiles_.begin(),
                         tiles_.end(),
                         tile.coord,
                         [](const TerrainWorldTile& t, TerrainTileCoord c) { return t.coord < c; });
    if (it != tiles_.end() && it->coord == tile.coord) {
        ++refusals_.duplicate_coords;
        return false;
    }
    tiles_.insert(it, std::move(tile));
    return true;
}

const TerrainWorldTile* TerrainWorld::find(TerrainTileCoord c) const noexcept {
    const auto it = std::lower_bound(
        tiles_.begin(), tiles_.end(), c, [](const TerrainWorldTile& t, TerrainTileCoord k) {
            return t.coord < k;
        });
    return it != tiles_.end() && it->coord == c ? &*it : nullptr;
}

std::vector<TerrainTileCoord> TerrainWorld::tiles_within(core::Vec3 p, float radius) const {
    std::vector<TerrainTileCoord> out;
    if (!(radius >= 0.0f) || !std::isfinite(p.x) || !std::isfinite(p.z)) {
        return out;
    }
    // The square of grid cells the radius can touch, then the exact rectangle-distance test. The
    // range is clamped so an absurd radius cannot overflow the i32 coordinates.
    const auto cell_range = [](float lo, float hi, float pitch) {
        const float a = std::clamp(std::floor(lo / pitch), -1.0e9f, 1.0e9f);
        const float b = std::clamp(std::floor(hi / pitch), -1.0e9f, 1.0e9f);
        return std::array<std::int32_t, 2>{static_cast<std::int32_t>(a),
                                           static_cast<std::int32_t>(b)};
    };
    const auto xs =
        cell_range(p.x - grid_.origin.x - radius, p.x - grid_.origin.x + radius, grid_.pitch_x());
    const auto zs =
        cell_range(p.z - grid_.origin.z - radius, p.z - grid_.origin.z + radius, grid_.pitch_z());
    // Walk the manifest's tiles when that is the smaller set (a huge radius over a small world).
    const std::int64_t cells =
        (std::int64_t{xs[1]} - xs[0] + 1) * (std::int64_t{zs[1]} - zs[0] + 1);
    if (cells > static_cast<std::int64_t>(tiles_.size())) {
        for (const TerrainWorldTile& t : tiles_) {
            if (grid_.distance_xz(t.coord, p) <= radius) {
                out.push_back(t.coord);
            }
        }
        return out;
    }
    for (std::int32_t z = zs[0]; z <= zs[1]; ++z) {
        for (std::int32_t x = xs[0]; x <= xs[1]; ++x) {
            const TerrainTileCoord c{x, z};
            if (find(c) != nullptr && grid_.distance_xz(c, p) <= radius) {
                out.push_back(c);
            }
        }
    }
    return out;
}

TerrainTileCheck
check_tile(const TerrainWorldGrid& grid, TerrainTileCoord c, const HeightfieldAsset& asset) {
    if (asset.columns != grid.samples || asset.rows != grid.samples ||
        asset.samples.size() != asset.sample_count()) {
        return TerrainTileCheck::SizeMismatch;
    }
    // Bit equality, deliberately: the cooker writes these as f32 from one authored value, and the
    // seam argument in the header is about EQUAL integers meaning EQUAL heights — "equal to within
    // a tolerance" would let a crack of exactly that tolerance through.
    if (asset.cell_size_x != grid.cell_size_x || asset.cell_size_z != grid.cell_size_z) {
        return TerrainTileCheck::SpacingMismatch;
    }
    if (asset.height_scale != grid.height_scale || asset.height_offset != grid.height_offset) {
        return TerrainTileCheck::QuantisationMismatch;
    }
    const core::Vec3 want = grid.tile_origin(c);
    if (!(std::fabs(asset.origin.x - want.x) <= kPlacementTolerance) ||
        !(std::fabs(asset.origin.y - want.y) <= kPlacementTolerance) ||
        !(std::fabs(asset.origin.z - want.z) <= kPlacementTolerance)) {
        return TerrainTileCheck::PlacementMismatch;
    }
    return TerrainTileCheck::Ok;
}

TerrainTileEdges tile_edges(const HeightfieldAsset& asset) {
    TerrainTileEdges e;
    if (asset.columns < 1 || asset.rows < 1 || asset.samples.size() != asset.sample_count()) {
        return e;
    }
    e.south.reserve(asset.columns);
    e.north.reserve(asset.columns);
    for (std::uint32_t i = 0; i < asset.columns; ++i) {
        e.south.push_back(asset.samples[asset.index(i, 0)]);
        e.north.push_back(asset.samples[asset.index(i, asset.rows - 1)]);
    }
    e.west.reserve(asset.rows);
    e.east.reserve(asset.rows);
    for (std::uint32_t j = 0; j < asset.rows; ++j) {
        e.west.push_back(asset.samples[asset.index(0, j)]);
        e.east.push_back(asset.samples[asset.index(asset.columns - 1, j)]);
    }
    return e;
}

bool edges_match(const TerrainTileEdges& a,
                 TerrainTileCoord a_coord,
                 const TerrainTileEdges& b,
                 TerrainTileCoord b_coord) noexcept {
    const std::int64_t dx = std::int64_t{b_coord.x} - a_coord.x;
    const std::int64_t dz = std::int64_t{b_coord.z} - a_coord.z;
    if (dx == 1 && dz == 0) {
        return a.east == b.west;
    }
    if (dx == -1 && dz == 0) {
        return a.west == b.east;
    }
    if (dx == 0 && dz == 1) {
        return a.north == b.south;
    }
    if (dx == 0 && dz == -1) {
        return a.south == b.north;
    }
    return true; // not edge-adjacent: nothing shared (a diagonal neighbour shares one corner,
                 // which both edge neighbours already pin)
}

} // namespace rime::assets
