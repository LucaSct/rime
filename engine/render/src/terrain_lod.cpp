// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Terrain LOD selection (m19.8d2). The header has the design and the derivation of the ranges;
// the notes here are about the balance pass and why its result does not depend on visiting order.

#include "rime/render/terrain_lod.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>

namespace rime::render {

namespace {

using assets::TerrainTileCoord;
using assets::TerrainTileKey;
using assets::TerrainWorld;

constexpr std::uint8_t kEdges[4] = {kTerrainEdgeNegX,
                                    kTerrainEdgePosX,
                                    kTerrainEdgeNegZ,
                                    kTerrainEdgePosZ};

constexpr float kInf = std::numeric_limits<float>::infinity();

std::array<TerrainTileKey, 4> children_of(TerrainTileKey k) {
    const std::uint32_t l = k.level - 1;
    const std::int32_t x = k.coord.x * 2;
    const std::int32_t z = k.coord.z * 2;
    return {{{l, {x, z}}, {l, {x + 1, z}}, {l, {x, z + 1}}, {l, {x + 1, z + 1}}}};
}

TerrainTileKey parent_of(TerrainTileKey k) {
    return {k.level + 1, assets::terrain_parent_coord(k.coord)};
}

// True when `k` lies under `ancestor` (or is it).
bool under(TerrainTileKey k, TerrainTileKey ancestor) {
    if (k.level > ancestor.level) {
        return false;
    }
    const std::uint32_t s = ancestor.level - k.level;
    return (k.coord.x >> s) == ancestor.coord.x && (k.coord.z >> s) == ancestor.coord.z;
}

} // namespace

TerrainTileKey terrain_lod_neighbour(TerrainTileKey k, std::uint8_t edge) noexcept {
    switch (edge) {
        case kTerrainEdgeNegX:
            return {k.level, {k.coord.x - 1, k.coord.z}};
        case kTerrainEdgePosX:
            return {k.level, {k.coord.x + 1, k.coord.z}};
        case kTerrainEdgeNegZ:
            return {k.level, {k.coord.x, k.coord.z - 1}};
        default:
            return {k.level, {k.coord.x, k.coord.z + 1}};
    }
}

double
terrain_lod_distance(const TerrainWorld& world, TerrainTileKey k, const core::Vec3& eye) noexcept {
    const assets::TerrainWorldGrid& g = world.grid();
    const assets::TerrainWorldTile* t = world.find(k);
    const core::Vec3 o = g.tile_origin(k);
    const double x0 = o.x;
    const double x1 = static_cast<double>(o.x) + static_cast<double>(g.pitch_x(k.level));
    const double z0 = o.z;
    const double z1 = static_cast<double>(o.z) + static_cast<double>(g.pitch_z(k.level));
    // A tile the manifest does not list has no bounds; such a key is never selected, and the
    // distance is the footprint's alone.
    const double y0 = t != nullptr ? t->min_y : eye.y;
    const double y1 = t != nullptr ? t->max_y : eye.y;
    // Point-to-box distance: clamp the point into the box, measure the offset.
    const double dx = std::max({x0 - eye.x, 0.0, eye.x - x1});
    const double dy = std::max({y0 - eye.y, 0.0, eye.y - y1});
    const double dz = std::max({z0 - eye.z, 0.0, eye.z - z1});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

TerrainLodRanges terrain_lod_ranges(const TerrainWorld& world, const TerrainLodView& view) {
    TerrainLodRanges r;
    const std::uint32_t levels = world.level_count();
    r.levels.resize(levels);
    const float tan_half = std::tan(0.5f * view.vertical_fov);
    r.error_to_distance = view.viewport_height_px / (2.0f * view.pixel_error * tan_half);
    const assets::TerrainWorldGrid& g = world.grid();
    for (std::uint32_t l = 0; l < levels; ++l) {
        TerrainLodLevel& lv = r.levels[l];
        const float px = g.pitch_x(l);
        const float pz = g.pitch_z(l);
        for (const assets::TerrainWorldTile& t : world.tiles(l)) {
            lv.error = std::max(lv.error, t.geometric_error);
            const float dy = t.max_y - t.min_y;
            lv.diagonal = std::max(lv.diagonal, std::sqrt(px * px + pz * pz + dy * dy));
        }
    }
    const float mu = view.step_margin;
    for (std::uint32_t l = 0; l + 1 < levels; ++l) {
        TerrainLodLevel& lv = r.levels[l];
        // (1) screen error: morph toward level l+1 only where l+1's error is within τ pixels.
        float start = r.error_to_distance * r.levels[l + 1].error;
        // (2) nesting: when a level-l node splits (camera within range_{l-1} of its box) none of
        // its own vertices may be morphing yet — and the step margin keeps that true for one frame
        // of camera motion either way.
        if (l > 0) {
            start = std::max(start, r.levels[l - 1].range + lv.diagonal + mu);
        }
        lv.morph_start = start;
        lv.morph_end = start + std::max(lv.diagonal, view.morph_fraction * start);
        // Children of a node that has just split are at least range − μ away: past morph_end.
        lv.range = lv.morph_end + mu;
    }
    if (levels > 0) {
        TerrainLodLevel& top = r.levels[levels - 1];
        top.morph_start = kInf;
        top.morph_end = kInf;
        top.range = kInf;
    }
    return r;
}

bool terrain_lod_ranges_valid(const TerrainLodRanges& r, const TerrainLodView& view) noexcept {
    const std::size_t n = r.levels.size();
    if (n == 0) {
        return false;
    }
    const float mu = view.step_margin;
    for (std::size_t l = 0; l + 1 < n; ++l) {
        const TerrainLodLevel& lv = r.levels[l];
        if (!(lv.morph_start >= r.error_to_distance * r.levels[l + 1].error) ||
            !(lv.morph_end > lv.morph_start) || !(lv.range >= lv.morph_end + mu)) {
            return false;
        }
        if (l > 0 && !(lv.morph_start >= r.levels[l - 1].range + lv.diagonal + mu)) {
            return false;
        }
    }
    return std::isinf(r.levels[n - 1].range);
}

TerrainLodSelection select_terrain_lod(const TerrainWorld& world,
                                       const TerrainLodRanges& ranges,
                                       const core::Vec3& eye,
                                       const TerrainLodUsable& usable) {
    TerrainLodSelection out;
    const std::uint32_t levels = world.level_count();
    if (levels == 0 || ranges.levels.size() < levels) {
        return out;
    }
    const std::uint32_t top = levels - 1;

    std::set<TerrainTileKey> internal; // split nodes
    std::set<TerrainTileKey> forced;   // split because the node itself is not drawable
    std::set<TerrainTileKey> wants;    // nodes whose range asked for their children

    // ── 1. The descent: split where the range asks AND the children can be drawn. ─────────────
    std::vector<TerrainTileKey> stack;
    for (const assets::TerrainWorldTile& t : world.tiles(top)) {
        stack.push_back(t.key());
    }
    while (!stack.empty()) {
        const TerrainTileKey n = stack.back();
        stack.pop_back();
        if (n.level == 0) {
            continue;
        }
        const bool range_wants = terrain_lod_distance(world, n, eye) <=
                                 static_cast<double>(ranges.levels[n.level - 1].range);
        const bool drawable = usable(n);
        if (range_wants) {
            wants.insert(n);
        }
        if (!range_wants && drawable) {
            continue;
        }
        const std::array<TerrainTileKey, 4> kids = children_of(n);
        const bool kids_ok = std::all_of(kids.begin(), kids.end(), [&](TerrainTileKey c) {
            return world.find(c) != nullptr && usable(c);
        });
        if (!kids_ok) {
            continue; // a fallback (or, for an undrawable root, a hole — counted below)
        }
        internal.insert(n);
        if (!drawable) {
            forced.insert(n);
        }
        for (const TerrainTileKey c : kids) {
            stack.push_back(c);
        }
    }

    // A node is IN the tree when it is a root or its parent split.
    const auto is_node = [&](TerrainTileKey k) {
        return k.level == top || internal.contains(parent_of(k));
    };

    // ── 2. Balance: collapse every split node that has an edge neighbour inside a coarser leaf. ──
    //
    // Why collapsing y fixes the violation it is visited for: y's children are finer than y and
    // the neighbour region r (same level as y) lies inside a leaf coarser than y, so the leaves on
    // either side of that edge differ by two or more. Collapsing y makes y the leaf on its side:
    // one level from r's leaf, or still more — which the next sweep catches. Every collapse is of a
    // node no balanced subtree of the descent can split (its neighbour is not in the tree, and
    // nothing ever re-splits), so the fixed point is the largest balanced subtree whatever the
    // order: the counts are deterministic as well as the leaves.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = internal.begin(); it != internal.end();) {
            const TerrainTileKey y = *it;
            bool violates = false;
            for (const std::uint8_t e : kEdges) {
                const TerrainTileKey r = terrain_lod_neighbour(y, e);
                if (world.find(r) != nullptr && !is_node(r)) {
                    violates = true;
                    break;
                }
            }
            if (!violates) {
                ++it;
                continue;
            }
            if (forced.contains(y)) {
                ++out.unbalanced;
                ++it;
                continue;
            }
            // Collapse y: drop it and every split node beneath it. `under` includes y itself.
            ++out.balance_collapses;
            changed = true;
            for (auto d = internal.begin(); d != internal.end();) {
                d = under(*d, y) ? internal.erase(d) : std::next(d);
            }
            it = internal.upper_bound(y);
        }
        if (changed) {
            out.unbalanced = 0; // recount on the next sweep, against the final tree
        }
    }

    // ── 3. The leaves, their coarser edges, and whether they are fallbacks. ─────────────────────
    stack.clear();
    for (const assets::TerrainWorldTile& t : world.tiles(top)) {
        stack.push_back(t.key());
    }
    while (!stack.empty()) {
        const TerrainTileKey n = stack.back();
        stack.pop_back();
        if (internal.contains(n)) {
            for (const TerrainTileKey c : children_of(n)) {
                stack.push_back(c);
            }
            continue;
        }
        if (!usable(n)) {
            ++out.uncovered; // only a root can get here (its parent's split required it usable)
            continue;
        }
        TerrainLodLeaf leaf;
        leaf.key = n;
        for (const std::uint8_t e : kEdges) {
            const TerrainTileKey r = terrain_lod_neighbour(n, e);
            // Not in the tree but in the world: r is inside a coarser leaf. (A split r means the
            // finer side is r's, which sets its own bit; a missing r is the world's edge.)
            if (world.find(r) != nullptr && !is_node(r)) {
                leaf.coarser_edges |= e;
            }
        }
        leaf.fallback = wants.contains(n);
        out.fallback_leaves += leaf.fallback ? 1u : 0u;
        out.leaves.push_back(leaf);
    }
    std::sort(out.leaves.begin(),
              out.leaves.end(),
              [](const TerrainLodLeaf& a, const TerrainLodLeaf& b) { return a.key < b.key; });
    return out;
}

} // namespace rime::render
