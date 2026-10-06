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
        case TerrainTileCheck::AlignmentMismatch:
            ++alignment_mismatches;
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
                // Two shapes: m19.8a's 8 fields (level 0, no error column) and m19.8d1's 10
                // (level after the record name, geometric error before the id).
                const auto tabs = std::count(line.begin(), line.end(), '\t');
                TerrainWorldTile t{};
                std::uint64_t id = 0;
                bool ok = false;
                std::string_view path;
                if (tabs == 7) {
                    std::array<std::string_view, 8> f{};
                    ok = world && split_tabs(line, f) && parse_int(f[1], t.coord.x) &&
                         parse_int(f[2], t.coord.z) && parse_int(f[3], t.revision) &&
                         parse_float(f[4], t.min_y) && parse_float(f[5], t.max_y) &&
                         parse_int(f[6], id, 16) && !f[7].empty();
                    path = f[7];
                } else if (tabs == 9) {
                    std::array<std::string_view, 10> f{};
                    ok = world && split_tabs(line, f) && parse_int(f[1], t.level) &&
                         t.level <= kMaxTerrainLevel && parse_int(f[2], t.coord.x) &&
                         parse_int(f[3], t.coord.z) && parse_int(f[4], t.revision) &&
                         parse_float(f[5], t.min_y) && parse_float(f[6], t.max_y) &&
                         parse_float(f[7], t.geometric_error) && t.geometric_error >= 0.0f &&
                         parse_int(f[8], id, 16) && !f[9].empty();
                    path = f[9];
                }
                if (!ok) {
                    RIME_ERROR("terrain world: line {}: malformed tile line (or no grid line "
                               "before it)",
                               line_number);
                    return std::nullopt;
                }
                t.id = AssetId{id};
                t.path = std::string(path);
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
        return world;
    }
    if (!world->validate_levels()) {
        RIME_WARN("terrain world: LOD chain refused (level 0 kept); see refusals()");
    }
    return world;
}

namespace {

[[nodiscard]] std::vector<TerrainWorldTile>::const_iterator
lower_bound_coord(const std::vector<TerrainWorldTile>& v, TerrainTileCoord c) {
    return std::lower_bound(
        v.begin(), v.end(), c, [](const TerrainWorldTile& t, TerrainTileCoord k) {
            return t.coord < k;
        });
}

} // namespace

bool TerrainWorld::add_tile(TerrainWorldTile tile) {
    if (tile.level > kMaxTerrainLevel) {
        ++refusals_.bad_levels;
        return false;
    }
    if (levels_.size() <= tile.level) {
        levels_.resize(tile.level + 1);
    }
    std::vector<TerrainWorldTile>& level = levels_[tile.level];
    const auto it = lower_bound_coord(level, tile.coord);
    if (it != level.end() && it->coord == tile.coord) {
        ++refusals_.duplicate_coords;
        return false;
    }
    level.insert(it, std::move(tile));
    return true;
}

bool TerrainWorld::validate_levels() {
    // Trailing empty levels (a resize for a tile that was then refused) are not part of the chain.
    while (levels_.size() > 1 && levels_.back().empty()) {
        levels_.pop_back();
    }
    if (levels_.size() == 1) {
        return true;
    }
    const std::uint64_t before =
        refusals_.odd_lod_samples + refusals_.missing_children + refusals_.uncovered_tiles;
    // Nesting takes every second sample of a child, which lands on the child's last sample only
    // when the child has an even number of cells.
    if ((grid_.samples - 1) % 2 != 0) {
        ++refusals_.odd_lod_samples;
    }
    const auto top = static_cast<std::uint32_t>(levels_.size() - 1);
    for (std::uint32_t l = 0; l <= top; ++l) {
        for (const TerrainWorldTile& t : levels_[l]) {
            // Shape check 1: a parent needs all four children. A parent of a partly present block
            // would have to invent the missing samples (a flat fill), which is a lie the renderer
            // would then morph a real child onto.
            if (l > 0) {
                bool complete = true;
                for (std::int32_t dz = 0; dz < 2; ++dz) {
                    for (std::int32_t dx = 0; dx < 2; ++dx) {
                        const TerrainTileCoord c{t.coord.x * 2 + dx, t.coord.z * 2 + dz};
                        complete = complete && find(TerrainTileKey{l - 1, c}) != nullptr;
                    }
                }
                if (!complete) {
                    ++refusals_.missing_children;
                }
            }
            // Shape check 2: everything below the top has a parent, so the top level — the root
            // cover a renderer pins — covers the whole world.
            if (l < top && find(TerrainTileKey{l + 1, terrain_parent_coord(t.coord)}) == nullptr) {
                ++refusals_.uncovered_tiles;
            }
        }
    }
    const std::uint64_t after =
        refusals_.odd_lod_samples + refusals_.missing_children + refusals_.uncovered_tiles;
    if (after == before) {
        return true;
    }
    levels_.resize(1);
    return false;
}

const std::vector<TerrainWorldTile>& TerrainWorld::tiles(std::uint32_t level) const noexcept {
    static const std::vector<TerrainWorldTile> kNone;
    return level < levels_.size() ? levels_[level] : kNone;
}

std::uint32_t TerrainWorld::level_count() const noexcept {
    auto n = static_cast<std::uint32_t>(levels_.size());
    while (n > 1 && levels_[n - 1].empty()) {
        --n;
    }
    return n;
}

const TerrainWorldTile* TerrainWorld::find(TerrainTileCoord c) const noexcept {
    return find(TerrainTileKey{0, c});
}

const TerrainWorldTile* TerrainWorld::find(TerrainTileKey k) const noexcept {
    if (k.level >= levels_.size()) {
        return nullptr;
    }
    const std::vector<TerrainWorldTile>& level = levels_[k.level];
    const auto it = lower_bound_coord(level, k.coord);
    return it != level.end() && it->coord == k.coord ? &*it : nullptr;
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
    if (cells > static_cast<std::int64_t>(tiles().size())) {
        for (const TerrainWorldTile& t : tiles()) {
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
    return check_tile(grid, TerrainTileKey{0, c}, asset);
}

TerrainTileCheck
check_tile(const TerrainWorldGrid& grid, TerrainTileKey k, const HeightfieldAsset& asset) {
    if (asset.columns != grid.samples || asset.rows != grid.samples ||
        asset.samples.size() != asset.sample_count()) {
        return TerrainTileCheck::SizeMismatch;
    }
    // Bit equality, deliberately: the cooker writes these as f32 from one authored value, and the
    // seam argument in the header is about EQUAL integers meaning EQUAL heights — "equal to within
    // a tolerance" would let a crack of exactly that tolerance through. A level's spacing is the
    // grid's times 2^L, which is exact in floating point (only the exponent moves), so bit
    // equality still holds at every level of a correctly cooked chain.
    if (k.level > kMaxTerrainLevel || asset.cell_size_x != grid.cell_size_x_at(k.level) ||
        asset.cell_size_z != grid.cell_size_z_at(k.level)) {
        return TerrainTileCheck::SpacingMismatch;
    }
    if (asset.height_scale != grid.height_scale || asset.height_offset != grid.height_offset) {
        return TerrainTileCheck::QuantisationMismatch;
    }
    const core::Vec3 want = grid.tile_origin(k);
    if (!(std::fabs(asset.origin.x - want.x) <= kPlacementTolerance) ||
        !(std::fabs(asset.origin.y - want.y) <= kPlacementTolerance) ||
        !(std::fabs(asset.origin.z - want.z) <= kPlacementTolerance)) {
        // Tell the two kinds of misplacement apart for a parent. On the level-0 lattice (a whole
        // number of source tiles from the grid origin) but not a multiple of 2^L of them: the cook
        // built this parent from a block that started on the wrong child. Anywhere else: an
        // ordinary placement error, as at level 0.
        if (k.level > 0 && std::fabs(asset.origin.y - want.y) <= kPlacementTolerance) {
            const double ux = (double{asset.origin.x} - grid.origin.x) / grid.pitch_x();
            const double uz = (double{asset.origin.z} - grid.origin.z) / grid.pitch_z();
            const double rx = std::round(ux);
            const double rz = std::round(uz);
            const bool on_lattice = std::fabs(ux - rx) * grid.pitch_x() <= kPlacementTolerance &&
                                    std::fabs(uz - rz) * grid.pitch_z() <= kPlacementTolerance;
            const double span = std::ldexp(1.0, static_cast<int>(k.level));
            if (on_lattice && (std::fmod(rx, span) != 0.0 || std::fmod(rz, span) != 0.0)) {
                return TerrainTileCheck::AlignmentMismatch;
            }
        }
        return TerrainTileCheck::PlacementMismatch;
    }
    return TerrainTileCheck::Ok;
}

bool samples_coincide(const HeightfieldAsset& parent,
                      TerrainTileKey parent_key,
                      const HeightfieldAsset& child,
                      TerrainTileKey child_key) noexcept {
    const std::uint32_t n = parent.columns;
    if (child_key.level + 1 != parent_key.level ||
        !(terrain_parent_coord(child_key.coord) == parent_key.coord) || n < 3 || n % 2 == 0 ||
        parent.rows != n || child.columns != n || child.rows != n ||
        parent.samples.size() != parent.sample_count() ||
        child.samples.size() != child.sample_count()) {
        return false;
    }
    // The child is one quadrant of the parent: `h` parent cells per axis, starting at parent sample
    // (qx·h, qz·h). Parent sample (qx·h + i, qz·h + j) sits on child sample (2i, 2j) — the nesting
    // the cook builds by taking every second sample.
    const std::uint32_t h = (n - 1) / 2;
    const auto qx = static_cast<std::uint32_t>(child_key.coord.x - parent_key.coord.x * 2);
    const auto qz = static_cast<std::uint32_t>(child_key.coord.z - parent_key.coord.z * 2);
    for (std::uint32_t j = 0; j <= h; ++j) {
        for (std::uint32_t i = 0; i <= h; ++i) {
            if (parent.samples[parent.index(qx * h + i, qz * h + j)] !=
                child.samples[child.index(2 * i, 2 * j)]) {
                return false;
            }
        }
    }
    return true;
}

TerrainLodVerification verify_lod_chain(
    const TerrainWorld& world,
    const std::function<std::optional<HeightfieldAsset>(const TerrainWorldTile&)>& load,
    TerrainWorldRefusals& refusals) {
    TerrainLodVerification v{};
    // Each tile is loaded once for its own check and once more per parent that compares it —
    // about twice the world, at most one parent and its four children in memory at a time. An
    // offline tool can afford that; holding a whole world's payloads could not be afforded.
    for (std::uint32_t l = 0; l < world.level_count(); ++l) {
        for (const TerrainWorldTile& t : world.tiles(l)) {
            const std::optional<HeightfieldAsset> asset = load(t);
            if (!asset) {
                ++refusals.unloadable_tiles;
                ++v.failures;
                continue;
            }
            ++v.tiles_checked;
            const TerrainTileCheck fit = check_tile(world.grid(), t.key(), *asset);
            if (fit != TerrainTileCheck::Ok) {
                refusals.count(fit);
                ++v.failures;
                continue; // a misfit parent's integers mean nothing to compare
            }
            if (l == 0) {
                continue;
            }
            for (std::int32_t dz = 0; dz < 2; ++dz) {
                for (std::int32_t dx = 0; dx < 2; ++dx) {
                    const TerrainTileKey ck{l - 1, {t.coord.x * 2 + dx, t.coord.z * 2 + dz}};
                    const TerrainWorldTile* ct = world.find(ck);
                    const std::optional<HeightfieldAsset> child =
                        ct != nullptr ? load(*ct) : std::nullopt;
                    if (!child) {
                        ++refusals.unloadable_tiles;
                        ++v.failures;
                        continue;
                    }
                    ++v.pairs_checked;
                    // The child's own fit is checked on its own turn; here it must at least share
                    // the quantisation, or equal integers would not be equal heights.
                    if (check_tile(world.grid(), ck, *child) != TerrainTileCheck::Ok ||
                        !samples_coincide(*asset, t.key(), *child, ck)) {
                        ++refusals.coincidence_mismatches;
                        ++v.failures;
                    }
                }
            }
        }
    }
    return v;
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
