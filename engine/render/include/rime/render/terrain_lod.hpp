// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "rime/assets/terrain_world.hpp"
#include "rime/core/math/vec.hpp"

// TERRAIN LOD SELECTION (m19.8d2, ADR-0071): which tiles of a world's LOD chain to draw this frame.
//
// m19.8d1 (ADR-0070) cooked the chain: a level-L tile is every second sample of its four
// level-(L−1) children, with the same sample count and twice the spacing, so a level-L tile at
// (x, z) is the quadtree node whose children are the four level-(L−1) tiles under it. This file is
// the CPU half of drawing that chain: given a camera, pick the set of nodes to draw. The GPU half —
// morphing a tile onto its parent so a switch never pops, and stitching edges — is terrain.vert.
//
// ── CDLOD (Strugar 2009, "Continuous Distance-Dependent Level of Detail") ─────────────────────
//
// Each level L gets a DISTANCE RANGE. A level-(L+1) node is split into its four children when the
// camera comes within `range_L` of the node's bounding box; otherwise it is drawn whole. Selection
// is a pure function of the camera, the ranges and which tiles are usable — no frame history, so
// two runs along the same path select the same sets (proof (f)).
//
// The ranges come from two requirements, derived in ADR-0071 and computed by `terrain_lod_ranges`:
//
//  1. SCREEN-SPACE ERROR. A vertical error e metres at distance d projects to
//     e · H / (2 · d · tan(fov/2)) pixels on a viewport H pixels tall. It is at most τ pixels when
//     d ≥ K · e, with K = H / (2 · τ · tan(fov/2)). A level-L tile's vertices start morphing toward
//     level L+1 at `morph_start_L`, so morph_start_L ≥ K · e_{L+1}, e being the level's largest
//     cooked geometric error (which 8d1 saturates up the chain, so it is monotone). A level-L tile
//     is only drawn unmorphed beyond range_{L−1} ≥ morph_start_{L−1} ≥ K · e_L, so its own error is
//     in budget too.
//  2. NESTING. When a level-(L+1) node splits, the camera is within range_L of its box, so every
//     vertex of it is within range_L + D_{L+1} (D = the level's largest box diagonal, height
//     included). Its own vertices must not be morphing at that moment — or the split would swap a
//     half-morphed parent for children drawn on the unmorphed one: a pop. Hence
//
//         morph_start_{L+1} ≥ range_L + D_{L+1} + μ,
//
//     μ being the largest camera step per frame the guarantee covers (`step_margin`). Its children
//     appear with every vertex at distance ≥ range_L − μ, and morph_end_L = range_L − μ, so they
//     appear FULLY morphed: exactly the parent's surface (terrain.vert's argument). The same
//     inequality is what bounds neighbours to one level apart: a leaf at level L sits in a split
//     node y at level L+1 within range_L of the camera; a neighbour leaf b at level ≥ L+2 is
//     unsplit, so all of b is farther than range_{L+1}; but y and b touch, so some point of b is
//     within range_L + D_{L+1} < range_{L+1}. Contradiction — so a leaf's neighbours are within one
//     level.
//
// ── RESIDENCY: A NODE DRAWS ITS CHILDREN ONLY IF ALL FOUR ARE USABLE ──────────────────────────
//
// `usable(key)` says whether a tile is resident and trusted. A node whose range asks for its
// children but whose children are not all usable is drawn itself — a FALLBACK: coarser than asked,
// never a hole. Every node the descent reaches was itself usable (that is what let its parent
// split), so any ancestor of a drawn leaf is drawable. Fallback can break the nesting argument
// above — a fallback node may sit next to a leaf two levels finer — so a BALANCE pass follows:
// while some split node y has a same-level edge neighbour that is not a node of the tree (it is
// inside a coarser leaf), y is collapsed into a leaf. Collapsing only ever coarsens, and the result
// is the unique largest 2:1-balanced subtree of the descent (the union of two balanced subtrees is
// balanced), so it does not depend on the order the violations are visited.
//
// The pass's counters tell the two apart: with every tile usable, `balance_collapses` must be 0 —
// the ranges alone guaranteed 2:1 — and the proofs assert exactly that, so a broken nesting
// inequality shows up as collapses rather than being silently repaired.
//
// ── EDGES ───────────────────────────────────────────────────────────────────────────────────────
//
// Each leaf carries four bits: "the neighbour across this edge is drawn coarser". terrain.vert
// forces those edge vertices fully onto the parent triangulation, which is exactly the coarse
// neighbour's edge — a T-junction with no gap (ADR-0071 bounds its f32 residual). With every tile
// usable the ranges already put those vertices at morph 1; the bits are what make fallback safe.
namespace rime::render {

// How the ranges are derived. Defaults: a 1080-pixel viewport at 60°, one pixel of error.
struct TerrainLodView {
    float viewport_height_px = 1080.0f;
    float vertical_fov = 1.0471976f; // radians
    float pixel_error = 1.0f;        // τ: the largest screen-space error accepted, in pixels
    // μ, metres: the largest camera step per frame for which a level switch is pop-free (and the
    // slack that absorbs the f32 difference between the CPU's box distance and the GPU's vertex
    // distance). A faster camera still never cracks — it may pop by the morph it skipped.
    float step_margin = 1.0f;
    // The morph region's width: max(D_L, morph_fraction · morph_start_L). A wider region morphs
    // more gently; it pushes every coarser range out by the same amount.
    float morph_fraction = 0.25f;
};

// One level's derived numbers. The top level never morphs and never splits: its morph_start,
// morph_end and range are +infinity.
struct TerrainLodLevel {
    float error = 0.0f;       // e_L: the largest cooked geometric_error among level-L tiles (m)
    float diagonal = 0.0f;    // D_L: the largest level-L bounding-box diagonal, height included (m)
    float morph_start = 0.0f; // a level-L vertex starts moving onto the level-(L+1) surface here
    float morph_end = 0.0f;   // ... and is on it here
    float range = 0.0f;       // a level-(L+1) node splits when its box is within this of the camera
};

struct TerrainLodRanges {
    std::vector<TerrainLodLevel> levels; // one per level of the world, finest first
    float error_to_distance = 0.0f;      // K: the distance at which 1 m of error is τ pixels
};

// Derive the per-level ranges from the world's manifest (errors, bounds) and the view. The
// formulas are the header's; ADR-0071 derives them.
[[nodiscard]] TerrainLodRanges terrain_lod_ranges(const assets::TerrainWorld& world,
                                                  const TerrainLodView& view);

// True when `r` satisfies every inequality the header names (screen error, nesting with
// `step_margin`, morph_end ≤ range − μ). A test checks a derived set; a hand-built set that fails
// is what the 2:1 proof's falsification uses.
[[nodiscard]] bool terrain_lod_ranges_valid(const TerrainLodRanges& r,
                                            const TerrainLodView& view) noexcept;

// The edge bits a leaf carries: the neighbour across that edge is drawn at a COARSER level.
enum TerrainLodEdge : std::uint8_t {
    kTerrainEdgeNegX = 1u << 0, // the edge at local i == 0
    kTerrainEdgePosX = 1u << 1, // i == samples − 1
    kTerrainEdgeNegZ = 1u << 2, // j == 0
    kTerrainEdgePosZ = 1u << 3, // j == samples − 1
};

struct TerrainLodLeaf {
    assets::TerrainTileKey key{};
    std::uint8_t coarser_edges = 0; // TerrainLodEdge bits
    // m19.8d3: the tile touching this leaf only at a CORNER is drawn coarser. Bit 0 = the corner
    // at local (0, 0), 1 = (last, 0), 2 = (0, last), 3 = (last, last). The geometry never needed
    // this — a corner is a parent vertex and does not move — but the APPEARANCE fade does: the
    // two same-level neighbours that share the corner along an edge with the coarse tile force it
    // to morph 1, and this leaf must agree with them or its shading differs from theirs there.
    std::uint8_t coarser_corners = 0;
    bool fallback = false; // the range asked for finer tiles here than are drawn
};

struct TerrainLodSelection {
    std::vector<TerrainLodLeaf> leaves;  // the tiles to draw, in key order (finest level first)
    std::uint32_t fallback_leaves = 0;   // leaves drawn coarser than their range asked
    std::uint32_t balance_collapses = 0; // split nodes collapsed to restore 2:1
    std::uint32_t unbalanced = 0;        // 2:1 violations that could not be collapsed (see below)
    std::uint32_t uncovered = 0;         // roots that are neither drawable nor splittable: holes
};

// Is this tile resident and trusted? (The residency's answer; a test passes its own.)
using TerrainLodUsable = std::function<bool(assets::TerrainTileKey)>;

// The 3D distance from `eye` to tile `k`'s bounding box: its footprint in XZ and [min_y, max_y]
// from the manifest (which bound every level-0 sample beneath it, so every vertex of the tile and
// of its descendants is inside). Zero inside. Computed in f64.
[[nodiscard]] double terrain_lod_distance(const assets::TerrainWorld& world,
                                          assets::TerrainTileKey k,
                                          const core::Vec3& eye) noexcept;

// Select the leaves to draw. Deterministic: a function of its arguments only.
//
// A ROOT (top-level tile) that is not usable is split anyway if its four children are; such a node
// cannot be collapsed by the balance pass (there is nothing to collapse it INTO) and a violation it
// causes is counted in `unbalanced`. A root that is neither is counted in `uncovered` and drawn by
// nobody. Neither happens while the roots are pinned and trusted (TerrainResidency's job).
[[nodiscard]] TerrainLodSelection select_terrain_lod(const assets::TerrainWorld& world,
                                                     const TerrainLodRanges& ranges,
                                                     const core::Vec3& eye,
                                                     const TerrainLodUsable& usable);

// The same-level neighbour of `k` across `edge` (one TerrainLodEdge bit).
[[nodiscard]] assets::TerrainTileKey terrain_lod_neighbour(assets::TerrainTileKey k,
                                                           std::uint8_t edge) noexcept;

} // namespace rime::render
