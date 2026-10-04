// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "epa.hpp"
#include "hull.hpp"
#include "narrowphase.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/shape.hpp"
#include "rime/physics/world.hpp"
#include "scene_query.hpp"

// Terrain heightfields (M19.1, ADR-0060-m19.1-heightfield) — the runtime store entry and all the
// geometry that runs against it: the ray walk, the per-triangle contact routines, and the patch
// grouping that turns a pile of triangle contacts into solver manifolds. PRIVATE, like every src/
// header.
//
// A heightfield is not convex, so none of the GJK/EPA machinery applies to it whole. It is, though,
// the most STRUCTURED non-convex shape there is: a regular grid, where "which triangles are near
// this point" is a floor() and never a tree search. Every routine here leans on that — the grid is
// its own midphase.
//
// THE TRIANGULATION (the one convention everything below agrees on, and which the cooked asset
// records): cell (ci, cj) has corners
//
//     v01 = (ci, cj+1) ---- v11 = (ci+1, cj+1)          local +Z
//        |  B        /  |                                  ^
//        |        /     |                                  |
//        |     /     A  |                                  +--> local +X
//     v00 = (ci, cj) ---- v10 = (ci+1, cj)
//
// split along the v00–v11 diagonal. In the cell's unit coordinates (u along X, v along Z) triangle
// A is u >= v (v00, v10, v11) and triangle B is u < v (v00, v11, v01). Triangle ids are
// 2 * (ci + (columns-1) * cj) + {0 for A, 1 for B}. heightfield_test.cpp's diagonal test is the
// witness: flip the split here and the ray hits a different surface.
namespace rime::physics {

namespace heightfield_detail {

// Hard ceiling per axis: triangle ids are u32, and 2 * 32767² < 2³², so every id fits.
inline constexpr std::uint32_t kMaxSamplesPerAxis = 32768;

// At most this many contact patches (manifolds) per body-vs-terrain pair: a body on terrain rough
// enough to need more is folded into the best-aligned patch and counted
// (WorldStats::heightfield_patch_merges).
inline constexpr std::size_t kMaxPatches = 4;

// Two triangle contacts join one patch when their normals are within ~3° (cos 3° ≈ 0.9986). Flat
// and planar-sloped ground is one patch (so a box on it is one 4-point manifold, exactly as on a
// box floor); a gully's two walls are two patches; a gently curved hill under a large box is a few.
inline constexpr float kPatchCos = 0.9986f;

// Two candidate contacts closer than this, with (almost) the same normal, are the same contact
// reported twice — a sphere over a shared edge or vertex, found by each triangle that owns it.
inline constexpr float kDedupDist2 = 1e-8f; // (0.1 mm)²
inline constexpr float kDedupCos = 0.9999f;

// Feature-id seed for terrain contacts ('HF').
inline constexpr std::uint32_t kFeatHeightfield = 0x48460001u;

} // namespace heightfield_detail

// One registered heightfield. Heights stay QUANTISED (u16 + scale/offset) exactly as registered:
// half the memory of floats, and the dequantisation is one multiply-add, so every query reads the
// same heights the cooked asset (and the renderer) holds.
struct HeightfieldShape {
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    float dx = 1.0f;
    float dz = 1.0f;
    float scale = 1.0f;
    float offset = 0.0f;
    float thickness = 1.0f;
    float min_h = 0.0f; // lowest / highest dequantised sample
    float max_h = 0.0f;
    std::vector<std::uint16_t> samples;

    [[nodiscard]] float h(std::uint32_t i, std::uint32_t j) const noexcept {
        return offset + scale * static_cast<float>(samples[i + columns * j]);
    }

    [[nodiscard]] std::uint32_t cells_x() const noexcept { return columns - 1; }

    [[nodiscard]] std::uint32_t cells_z() const noexcept { return rows - 1; }

    [[nodiscard]] float extent_x() const noexcept { return dx * static_cast<float>(columns - 1); }

    [[nodiscard]] float extent_z() const noexcept { return dz * static_cast<float>(rows - 1); }

    [[nodiscard]] std::uint32_t triangle_count() const noexcept {
        return 2 * cells_x() * cells_z();
    }

    [[nodiscard]] Aabb local_bounds() const noexcept {
        return Aabb{core::Vec3{0.0f, min_h - thickness, 0.0f},
                    core::Vec3{extent_x(), max_h, extent_z()}};
    }
};

// Validate + copy a HeightfieldDesc into a store entry. Rejects (false, `out` untouched) rather
// than repairing — the hull/compound registration posture.
[[nodiscard]] inline bool build_heightfield(const HeightfieldDesc& d, HeightfieldShape& out) {
    using heightfield_detail::kMaxSamplesPerAxis;
    const auto finite_pos = [](float v) { return std::isfinite(v) && v > 0.0f; };
    if (d.columns < 2 || d.rows < 2 || d.columns > kMaxSamplesPerAxis ||
        d.rows > kMaxSamplesPerAxis ||
        d.samples.size() != std::size_t{d.columns} * std::size_t{d.rows} ||
        !finite_pos(d.cell_size_x) || !finite_pos(d.cell_size_z) || !finite_pos(d.height_scale) ||
        !std::isfinite(d.height_offset) || !std::isfinite(d.thickness) || d.thickness < 0.0f) {
        return false;
    }
    const auto [lo, hi] = std::minmax_element(d.samples.begin(), d.samples.end());
    const float min_h = d.height_offset + d.height_scale * static_cast<float>(*lo);
    const float max_h = d.height_offset + d.height_scale * static_cast<float>(*hi);
    if (!std::isfinite(max_h) || !std::isfinite(min_h - d.thickness)) {
        return false;
    }
    HeightfieldShape hf;
    hf.columns = d.columns;
    hf.rows = d.rows;
    hf.dx = d.cell_size_x;
    hf.dz = d.cell_size_z;
    hf.scale = d.height_scale;
    hf.offset = d.height_offset;
    hf.thickness = d.thickness;
    hf.min_h = min_h;
    hf.max_h = max_h;
    hf.samples.assign(d.samples.begin(), d.samples.end());
    out = std::move(hf);
    return true;
}

// World bound of a posed heightfield: the eight corners of the local box, posed. This is the ONE
// broadphase proxy a terrain tile owns (see the ADR on why one big static leaf is acceptable).
[[nodiscard]] inline Aabb
heightfield_world_aabb(const HeightfieldShape& hf, core::Vec3 pos, const core::Quat& q) noexcept {
    const Aabb l = hf.local_bounds();
    core::Vec3 lo{std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max()};
    core::Vec3 hi = -lo;
    for (int k = 0; k < 8; ++k) {
        const core::Vec3 c{(k & 1) != 0 ? l.max.x : l.min.x,
                           (k & 2) != 0 ? l.max.y : l.min.y,
                           (k & 4) != 0 ? l.max.z : l.min.z};
        const core::Vec3 w = pos + core::rotate(q, c);
        lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
        hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
    }
    return Aabb{lo, hi};
}

// One cell's four corner heights and the two triangle planes they define, in CELL-LOCAL metres
// (x, z measured from the cell's v00 corner). Each triangle's surface is the plane
//     y = base + sx * x + sz * z
// — the heightfield's defining property: over a triangle, height is linear in (x, z). That is what
// makes every per-triangle test below exact rather than iterative.
struct HeightfieldCell {
    std::uint32_t ci = 0;
    std::uint32_t cj = 0;
    float h00 = 0.0f, h10 = 0.0f, h01 = 0.0f, h11 = 0.0f;
    float dx = 1.0f;
    float dz = 1.0f;

    // Plane slopes of triangle A (u >= v: v00, v10, v11) and B (u < v: v00, v11, v01).
    [[nodiscard]] float sx(int half) const noexcept {
        return half == 0 ? (h10 - h00) / dx : (h11 - h01) / dx;
    }

    [[nodiscard]] float sz(int half) const noexcept {
        return half == 0 ? (h11 - h10) / dz : (h01 - h00) / dz;
    }

    // Which triangle holds cell-local (x, z): u >= v ⇔ x/dx >= z/dz ⇔ x*dz >= z*dx (no division).
    [[nodiscard]] int half_at(float x, float z) const noexcept { return x * dz >= z * dx ? 0 : 1; }

    // Height of triangle `half`'s PLANE at cell-local (x, z) — valid anywhere, exact on the
    // triangle.
    [[nodiscard]] float plane_y(int half, float x, float z) const noexcept {
        return h00 + sx(half) * x + sz(half) * z;
    }

    // Unit upward normal of triangle `half`: the gradient form (-sx, 1, -sz), normalised.
    [[nodiscard]] core::Vec3 normal(int half) const noexcept {
        return core::normalize(core::Vec3{-sx(half), 1.0f, -sz(half)});
    }

    [[nodiscard]] float min_y() const noexcept { return std::min({h00, h10, h01, h11}); }

    [[nodiscard]] float max_y() const noexcept { return std::max({h00, h10, h01, h11}); }
};

[[nodiscard]] inline HeightfieldCell
cell_of(const HeightfieldShape& hf, std::uint32_t ci, std::uint32_t cj) noexcept {
    HeightfieldCell c;
    c.ci = ci;
    c.cj = cj;
    c.h00 = hf.h(ci, cj);
    c.h10 = hf.h(ci + 1, cj);
    c.h01 = hf.h(ci, cj + 1);
    c.h11 = hf.h(ci + 1, cj + 1);
    c.dx = hf.dx;
    c.dz = hf.dz;
    return c;
}

// The three LOCAL-frame vertices of triangle `half` of a cell, in the winding the diagram shows,
// and their global vertex ids (i + columns * j) — the stable names feature ids are built from.
inline void triangle_vertices(const HeightfieldShape& hf,
                              const HeightfieldCell& c,
                              int half,
                              core::Vec3 v[3],
                              std::uint32_t vid[3]) noexcept {
    const float x0 = hf.dx * static_cast<float>(c.ci);
    const float z0 = hf.dz * static_cast<float>(c.cj);
    const core::Vec3 p00{x0, c.h00, z0};
    const core::Vec3 p10{x0 + hf.dx, c.h10, z0};
    const core::Vec3 p11{x0 + hf.dx, c.h11, z0 + hf.dz};
    const core::Vec3 p01{x0, c.h01, z0 + hf.dz};
    const std::uint32_t i00 = c.ci + hf.columns * c.cj;
    const std::uint32_t i10 = i00 + 1;
    const std::uint32_t i01 = i00 + hf.columns;
    const std::uint32_t i11 = i01 + 1;
    if (half == 0) {
        v[0] = p00, v[1] = p10, v[2] = p11;
        vid[0] = i00, vid[1] = i10, vid[2] = i11;
    } else {
        v[0] = p00, v[1] = p11, v[2] = p01;
        vid[0] = i00, vid[1] = i11, vid[2] = i01;
    }
}

[[nodiscard]] inline std::uint32_t
triangle_id(const HeightfieldShape& hf, std::uint32_t ci, std::uint32_t cj, int half) noexcept {
    return 2u * (ci + hf.cells_x() * cj) + static_cast<std::uint32_t>(half);
}

// The inclusive cell index range an interval [lo, hi] covers along one axis, clamped to the grid.
// floor() of each end over the cell size — the grid IS its own midphase, so there is no tree to
// descend. Shared by the contact build and the convex queries (m19.2) so there is exactly one
// definition of "which cells does this bound touch": two copies would be two chances to disagree
// about a bound that lands exactly on a grid line, and a query that enumerated a different cell
// set from the narrowphase would report hits the solver never sees.
inline void cell_range(float lo,
                       float hi,
                       float size,
                       std::uint32_t cells,
                       std::uint32_t& c0,
                       std::uint32_t& c1) noexcept {
    const float f0 = std::floor(lo / size);
    const float f1 = std::floor(hi / size);
    c0 = f0 <= 0.0f ? 0u : static_cast<std::uint32_t>(std::min(f0, float(cells - 1)));
    c1 = f1 <= 0.0f ? 0u : static_cast<std::uint32_t>(std::min(f1, float(cells - 1)));
}

// ─── The ray walk ────────────────────────────────────────────────────────────────────────────
//
// A ray against a heightfield is a 2-D problem wearing a 3-D coat. Project the ray onto the XZ
// plane and it crosses a sequence of grid cells; the surface can only be hit inside one of them,
// and the FIRST hit along that sequence is the nearest (cells are visited in increasing t). So:
//
//  1. Clip the ray to the heightfield's local box (the slab test): outside it nothing can be hit,
//     which is also what makes "ray outside the bounds" a clean miss rather than a clamped cell.
//  2. Walk the cells the projected ray crosses with a 2-D DDA — Amanatides & Woo's "fast voxel
//     traversal": keep, per axis, the t at which the ray next crosses a grid line, and always step
//     across whichever comes first. Each step is O(1) and no cell is skipped or visited twice. The
//     crossing t is recomputed from the cell index each step (rather than accumulated by adding
//     tDelta) so float error cannot drift across a long walk.
//  3. In each cell, the ray's segment [ta, tb] is split where it crosses the cell's diagonal; on
//     each piece the surface is ONE triangle's plane, so the ray's height above it,
//         f(t) = y(t) - plane(x(t), z(t)),
//     is linear in t. A hit is where f goes from >= 0 to <= 0 — one division, exact up to
//     rounding. That is the "exact per-triangle test": a ray–plane intersection restricted to the
//     part of the ray that is over that triangle.
//
// WATERTIGHTNESS. Two neighbouring triangles evaluate their shared edge from two different plane
// equations, which can disagree by an ulp — so f could read +ε at the end of one piece and −ε at
// the start of the next, a sign change that belongs to NEITHER piece, and a ray aimed exactly at an
// edge or a vertex would slip through the seam. The walk therefore carries the previous piece's
// end value forward: "was above at the end of the last piece, is below at the start of this one"
// is itself a hit, at the seam. That makes the surface closed by construction — no epsilon, no
// "fatten the triangles".
//
// The sweep test does not reach it, and saying so is the honest version. Instrumenting the branch
// with a counter and running the whole physics suite (167 cases, including "the surface is
// watertight at every edge and vertex" and its 3,300 casts at vertices, edge midpoints and
// diagonal midpoints of a bumpy non-planar tile) fires it **zero** times: every one of those rays
// is steep enough that f moves by far more than an ulp across each piece, so the ordinary
// `fa >= 0 && fb <= 0` root is what closes the surface there. Provoking the seam needs a ray
// within an ulp of grazing at a shared edge AND the two plane equations rounding in opposite
// directions at that exact t -- not constructible by hand, but FINDABLE: a seeded search over ~4M
// grazing rays fires it 12 times, and "the seam rule closes a surface the per-piece root alone
// leaks" pins five of them bit-exactly (each misses with the rule deleted). That pin is tied to
// float rounding, so it is evidence for this toolchain, not a proof for every one. Measured
// 2026-10-04.
//
// ONE-SIDED: a ray that starts BELOW the surface never reports the surface as it rises out (f goes
// negative → positive, which is not a hit). The same rule ray_vs_box uses for an origin inside the
// box. A cheap per-cell cull skips cells whose four corners lie entirely below or above the ray's
// segment over that cell (the plane can only be hit between its min and max height).
//
// `o`/`d` are in the heightfield's LOCAL frame and `d` is unit, so t is a distance in metres.
#ifdef RIME_PHYSICS_SEAM_COUNTER
// TEST-ONLY HOOK (defined solely for rime_physics_tests, see tests/physics/CMakeLists.txt). The
// seam rule below never fires on ordinary rays, so a test that merely checks "the ray hits" cannot
// tell a rule that closed the seam from a toolchain whose rounding no longer reaches it -- the
// proof would lapse into passing silently. This counts firings so the test can SEE the rule act.
// The definitions live in an INLINE NAMESPACE so this instrumented copy of the walk has different
// symbols from the library's uninstrumented one (same header, same inline names, two bodies in one
// binary would be an ODR violation the linker resolves arbitrarily). Without the macro there is no
// counter, no atomic and no extra branch: the hot path is byte-for-byte the plain one.
inline namespace seam_counted {
inline std::atomic<std::uint64_t> g_seam_firings{0};

[[nodiscard]] inline std::uint64_t seam_firings() noexcept {
    return g_seam_firings.load(std::memory_order_relaxed);
}
#endif
[[nodiscard]] inline bool ray_vs_heightfield_local(const HeightfieldShape& hf,
                                                   core::Vec3 o,
                                                   core::Vec3 d,
                                                   float tmax,
                                                   float& t_out,
                                                   core::Vec3& n_out,
                                                   std::uint32_t& tri_out) noexcept {
    // 1. Slab clip against the SURFACE's box (the thickness band below min_h is solid for
    //    contacts, but a ray only ever hits the top surface, which lives in [min_h, max_h]).
    //    Padded vertically: a ray whose hit is exactly ON the box's floor or ceiling (a vertical
    //    ray onto the lowest sample) would otherwise have its walk end at a t whose rounded y
    //    sits a hair ABOVE the surface, and the root would fall just outside the walk. The pad
    //    cannot create a hit — hits come only from the surface test — it only lets the walk run
    //    long enough to see one. (The watertight sweep in heightfield_test.cpp found this.)
    const float pad = 1e-3f * (1.0f + std::max(std::fabs(hf.min_h), std::fabs(hf.max_h)));
    const float lo[3] = {0.0f, hf.min_h - pad, 0.0f};
    const float hi[3] = {hf.extent_x(), hf.max_h + pad, hf.extent_z()};
    const float oo[3] = {o.x, o.y, o.z};
    const float dd[3] = {d.x, d.y, d.z};
    float t0 = 0.0f;
    float t1 = tmax;
    for (int a = 0; a < 3; ++a) {
        if (dd[a] == 0.0f) {
            if (oo[a] < lo[a] || oo[a] > hi[a]) {
                return false; // parallel to this slab and outside it
            }
            continue;
        }
        const float inv = 1.0f / dd[a];
        float ta = (lo[a] - oo[a]) * inv;
        float tb = (hi[a] - oo[a]) * inv;
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) {
            return false;
        }
    }

    // 2. DDA set-up. The entry point's cell, clamped so a ray entering exactly on the far edge
    //    (floor(extent/dx) == cells) starts in the last real cell.
    const auto clamp_cell = [](float c, std::uint32_t cells) {
        const float f = std::floor(c);
        if (!(f >= 0.0f)) {
            return std::int64_t{0};
        }
        return std::min(static_cast<std::int64_t>(f), static_cast<std::int64_t>(cells) - 1);
    };
    std::int64_t ci = clamp_cell((o.x + d.x * t0) / hf.dx, hf.cells_x());
    std::int64_t cj = clamp_cell((o.z + d.z * t0) / hf.dz, hf.cells_z());
    const int step_x = d.x > 0.0f ? 1 : (d.x < 0.0f ? -1 : 0);
    const int step_z = d.z > 0.0f ? 1 : (d.z < 0.0f ? -1 : 0);
    constexpr float kInf = std::numeric_limits<float>::infinity();
    // t at which the ray leaves cell column `c` along X (the next grid line in its direction).
    const auto exit_x = [&](std::int64_t c) {
        if (step_x == 0) {
            return kInf;
        }
        const std::int64_t line = step_x > 0 ? c + 1 : c;
        return (hf.dx * static_cast<float>(line) - o.x) / d.x;
    };
    const auto exit_z = [&](std::int64_t c) {
        if (step_z == 0) {
            return kInf;
        }
        const std::int64_t line = step_z > 0 ? c + 1 : c;
        return (hf.dz * static_cast<float>(line) - o.z) / d.z;
    };

    bool have_prev = false;
    float prev_f = 0.0f; // the ray's height above the surface at the end of the previous piece

    // One linear piece [a, b] over triangle `half`: the exact root, or the seam rule.
    const auto piece = [&](const HeightfieldCell& c, int half, float a, float b) -> bool {
        const float x0 = hf.dx * static_cast<float>(c.ci);
        const float z0 = hf.dz * static_cast<float>(c.cj);
        const auto f = [&](float t) {
            return (o.y + d.y * t) - c.plane_y(half, o.x + d.x * t - x0, o.z + d.z * t - z0);
        };
        const float fa = f(a);
        const float fb = f(b);
        float hit_t = -1.0f;
        if (have_prev && prev_f > 0.0f && fa < 0.0f) {
#ifdef RIME_PHYSICS_SEAM_COUNTER
            g_seam_firings.fetch_add(1, std::memory_order_relaxed);
#endif
            hit_t = a; // crossed down exactly on the seam between the last piece and this one
        } else if (fa >= 0.0f && fb <= 0.0f) {
            // f is linear on [a, b]: its root is at the fraction fa / (fa - fb). fa == fb == 0 is
            // a ray lying IN the plane — touching from its first point, so it hits at a.
            hit_t = fa > fb ? a + (b - a) * (fa / (fa - fb)) : a;
        }
        prev_f = fb;
        have_prev = true;
        if (hit_t < 0.0f) {
            return false;
        }
        t_out = hit_t;
        n_out = c.normal(half);
        tri_out = triangle_id(hf, c.ci, c.cj, half);
        return true;
    };

    float ta = t0;
    for (;;) {
        const float ex = exit_x(ci);
        const float ez = exit_z(cj);
        const float tb = std::max(ta, std::min({ex, ez, t1}));
        const HeightfieldCell c =
            cell_of(hf, static_cast<std::uint32_t>(ci), static_cast<std::uint32_t>(cj));

        // Cull: if the ray's whole segment over this cell is above the cell's highest corner (or
        // below its lowest), no triangle here can be crossed. Keep the seam state honest.
        const float ya = o.y + d.y * ta;
        const float yb = o.y + d.y * tb;
        if (std::min(ya, yb) > c.max_y()) {
            prev_f = 1.0f;
            have_prev = true;
        } else if (std::max(ya, yb) < c.min_y()) {
            prev_f = -1.0f;
            have_prev = true;
        } else {
            // 3. Split at the diagonal. g(t) = x*dz - z*dx (cell-local) is >= 0 over triangle A;
            //    it is linear in t, so it changes sign at most once inside the segment.
            const float x0 = hf.dx * static_cast<float>(c.ci);
            const float z0 = hf.dz * static_cast<float>(c.cj);
            const auto g = [&](float t) {
                return (o.x + d.x * t - x0) * hf.dz - (o.z + d.z * t - z0) * hf.dx;
            };
            const float ga = g(ta);
            const float gb = g(tb);
            if ((ga > 0.0f && gb < 0.0f) || (ga < 0.0f && gb > 0.0f)) {
                const float tc = ta + (tb - ta) * (ga / (ga - gb));
                if (piece(c, ga >= 0.0f ? 0 : 1, ta, tc) || piece(c, gb >= 0.0f ? 0 : 1, tc, tb)) {
                    return true;
                }
            } else {
                // No strict crossing: the whole piece is on one side (or ON the diagonal, where
                // both planes agree — pick by the midpoint).
                const int half = g(0.5f * (ta + tb)) >= 0.0f ? 0 : 1;
                if (piece(c, half, ta, tb)) {
                    return true;
                }
            }
        }

        if (tb >= t1) {
            return false;
        }
        // Step across whichever grid line comes first; both at once through a grid VERTEX (the
        // ray touches the two side cells only at that point, which the seam rule already covers).
        if (ex < ez) {
            ci += step_x;
        } else if (ez < ex) {
            cj += step_z;
        } else {
            ci += step_x;
            cj += step_z;
        }
        ta = tb;
        if (ci < 0 || cj < 0 || ci >= static_cast<std::int64_t>(hf.cells_x()) ||
            cj >= static_cast<std::int64_t>(hf.cells_z())) {
            return false; // walked off the grid
        }
    }
}

// World-space wrapper: rotate the ray into the heightfield's frame (an isometry — t is unchanged),
// walk, rotate the normal back out.
[[nodiscard]] inline bool ray_vs_heightfield(const HeightfieldShape& hf,
                                             core::Vec3 pos,
                                             const core::Quat& q,
                                             core::Vec3 o,
                                             core::Vec3 d,
                                             float tmax,
                                             float& t_out,
                                             core::Vec3& n_out) noexcept {
    const core::Quat qc = core::conjugate(q);
    std::uint32_t tri = 0;
    core::Vec3 n{0.0f, 1.0f, 0.0f};
    if (!ray_vs_heightfield_local(
            hf, core::rotate(qc, o - pos), core::rotate(qc, d), tmax, t_out, n, tri)) {
        return false;
    }
    n_out = core::rotate(q, n);
    return true;
}
#ifdef RIME_PHYSICS_SEAM_COUNTER
} // namespace seam_counted
#endif

// ─── Contacts ────────────────────────────────────────────────────────────────────────────────
//
// Body-vs-terrain contacts are generated triangle by triangle over the cells under the body's
// bound, then GROUPED into manifolds. Every per-triangle routine treats the triangle as ONE-SIDED
// with the terrain solid beneath it: a contact normal is either the triangle's own upward normal
// or (for a round shape touching an edge/vertex from above) the radial direction, never a
// sideways "internal edge" normal. That is the classic heightfield/trimesh failure the approach
// is chosen to avoid — a box sliding across a flat terrain must not catch on the seams between
// triangles, which a GJK/EPA test against each triangle as a separate convex solid would do.

// One candidate contact, in the heightfield's LOCAL frame.
struct HeightfieldContact {
    core::Vec3 point;  // midway between the two surfaces (the contact.hpp convention)
    core::Vec3 normal; // unit, from the TERRAIN toward the body
    float depth = 0.0f;
    std::uint32_t feature = 0;
    std::uint32_t triangle = 0;
};

namespace heightfield_detail {

// Closest point on triangle (a, b, c) to p — Ericson, Real-Time Collision Detection §5.1.5: walk
// the Voronoi regions (three vertices, three edges, the face) with barycentric sign tests. Also
// reports WHICH feature won: 0 = face, 1..3 = vertex a/b/c, 4..6 = edge ab/bc/ca — the contact's
// feature id, and the thing that says whether a point is over the triangle's interior.
inline core::Vec3
closest_on_triangle(core::Vec3 p, core::Vec3 a, core::Vec3 b, core::Vec3 c, int& feature) noexcept {
    const core::Vec3 ab = b - a;
    const core::Vec3 ac = c - a;
    const core::Vec3 ap = p - a;
    const float d1 = core::dot(ab, ap);
    const float d2 = core::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        feature = 1;
        return a;
    }
    const core::Vec3 bp = p - b;
    const float d3 = core::dot(ab, bp);
    const float d4 = core::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        feature = 2;
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        feature = 4;
        return a + ab * (d1 / (d1 - d3));
    }
    const core::Vec3 cp = p - c;
    const float d5 = core::dot(ab, cp);
    const float d6 = core::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        feature = 3;
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        feature = 6;
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        feature = 5;
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    feature = 0;
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// A STABLE name for the triangle feature a contact came from: the face by triangle id; a vertex by
// its global vertex id; an edge by its two global vertex ids (sorted). Two triangles sharing an
// edge name it identically, which is what lets a contact keep its warm-start id as a body rolls
// from one triangle onto the next across that edge.
inline std::uint32_t
feature_name(int feature, std::uint32_t tri, const std::uint32_t vid[3]) noexcept {
    switch (feature) {
        case 0:
            return feature_combine(0xFACEu, tri);
        case 1:
        case 2:
        case 3:
            return feature_combine(0x7E27u, vid[feature - 1]);
        default: {
            const std::uint32_t e0 = vid[feature - 4];
            const std::uint32_t e1 = vid[(feature - 4 + 1) % 3];
            return feature_combine(feature_combine(0xED6Eu, std::min(e0, e1)), std::max(e0, e1));
        }
    }
}

inline void push_unique(std::vector<HeightfieldContact>& out, const HeightfieldContact& c) {
    for (const HeightfieldContact& e : out) {
        const core::Vec3 dp = e.point - c.point;
        if (core::dot(dp, dp) <= kDedupDist2 && core::dot(e.normal, c.normal) >= kDedupCos) {
            return; // the same contact, found again by a neighbouring triangle
        }
    }
    out.push_back(c);
}

// A sphere (centre `c`, radius `r`) against one triangle. Above the triangle's plane: the closest
// point on the triangle, within r ⇒ contact along the radial direction (the face normal when the
// closest point is interior, the edge/vertex radial otherwise). Below the plane — sunk into the
// ground — only if the centre's projection lies over the triangle's interior, and then along the
// triangle's normal: straight back up, never sideways. A centre below the plane and outside the
// triangle belongs to a neighbour (whose face region does contain it).
inline void sphere_vs_triangle(core::Vec3 c,
                               float r,
                               const core::Vec3 v[3],
                               const std::uint32_t vid[3],
                               core::Vec3 n_tri,
                               std::uint32_t tri,
                               std::uint32_t seed,
                               std::vector<HeightfieldContact>& out) {
    int feature = 0;
    const core::Vec3 q = closest_on_triangle(c, v[0], v[1], v[2], feature);
    const float s = core::dot(c - v[0], n_tri); // signed height above the plane
    core::Vec3 n;
    float depth = 0.0f;
    if (s < 0.0f) {
        if (feature != 0) {
            return;
        }
        n = n_tri;
        depth = r - s;
    } else {
        const core::Vec3 delta = c - q;
        const float d2 = core::dot(delta, delta);
        if (d2 >= r * r) {
            return;
        }
        const float dist = std::sqrt(d2);
        n = dist > narrowphase_detail::kNormalEps ? delta * (1.0f / dist) : n_tri;
        depth = r - dist;
    }
    HeightfieldContact k;
    const core::Vec3 body_surface = c - n * r;
    k.point = body_surface + n * (0.5f * depth);
    k.normal = n;
    k.depth = depth;
    k.feature = feature_combine(seed, feature_name(feature, tri, vid));
    k.triangle = tri;
    push_unique(out, k);
}

// A capsule (core segment p0–p1, radius r) against one triangle: its two end spheres (which carry
// a capsule lying flat — two points, so it does not seesaw), plus the closest approach of the core
// segment to each triangle edge (a capsule lying ACROSS a ridge touches it mid-shaft, where neither
// end sphere reaches). Edge contacts count only from above the plane, like the sphere's.
inline void capsule_vs_triangle(core::Vec3 p0,
                                core::Vec3 p1,
                                float r,
                                const core::Vec3 v[3],
                                const std::uint32_t vid[3],
                                core::Vec3 n_tri,
                                std::uint32_t tri,
                                std::uint32_t seed,
                                std::vector<HeightfieldContact>& out) {
    sphere_vs_triangle(p0, r, v, vid, n_tri, tri, feature_combine(seed, 1u), out);
    sphere_vs_triangle(p1, r, v, vid, n_tri, tri, feature_combine(seed, 2u), out);
    for (int e = 0; e < 3; ++e) {
        float s = 0.0f;
        float t = 0.0f;
        core::Vec3 on_seg;
        core::Vec3 on_edge;
        narrowphase_detail::closest_segment_segment(
            p0, p1, v[e], v[(e + 1) % 3], s, t, on_seg, on_edge);
        if (s <= 0.0f || s >= 1.0f || core::dot(on_seg - v[0], n_tri) < 0.0f) {
            continue; // an end sphere already covers the ends; below the plane is the face's job
        }
        const core::Vec3 delta = on_seg - on_edge;
        const float d2 = core::dot(delta, delta);
        if (d2 >= r * r || d2 <= narrowphase_detail::kNormalEps * narrowphase_detail::kNormalEps) {
            continue;
        }
        const float dist = std::sqrt(d2);
        const core::Vec3 n = delta * (1.0f / dist);
        HeightfieldContact k;
        const float depth = r - dist;
        k.point = (on_seg - n * r) + n * (0.5f * depth);
        k.normal = n;
        k.depth = depth;
        k.feature = feature_combine(feature_combine(seed, 3u), feature_name(4 + e, tri, vid));
        k.triangle = tri;
        push_unique(out, k);
    }
}

// A convex polyhedron (box or hull, as a PolyView) against one triangle. The triangle is ALWAYS
// the reference face — the move that keeps the normal one-sided: it is the triangle's normal, not
// whatever EPA would have picked. Two sources of points:
//  - REFERENCE-FACE CLIPPING, as the convex path does it (narrowphase.hpp): the body's face most
//    opposed to the triangle normal is clipped by the triangle's three side planes (vertical
//    walls through its edges), and whatever survives below the triangle's plane is a contact.
//    This is what supports a box LARGER than a triangle: its corners are far outside most
//    triangles, but the clipped face still produces points over each one.
//  - VERTICES in the triangle's prism and below its plane — the case clipping one face misses (a
//    box tipped onto an edge, a hull corner pushed into the ground).
// Depth is measured along the triangle normal; duplicates between the two sources collapse.
inline void poly_vs_triangle(const narrowphase_detail::PolyView& poly,
                             const core::Vec3 v[3],
                             core::Vec3 n_tri,
                             std::uint32_t tri,
                             std::uint32_t seed,
                             std::vector<HeightfieldContact>& out) {
    using narrowphase_detail::ClipVertex;
    const narrowphase_detail::PolySupport sup{&poly};
    const float plane_d = core::dot(n_tri, v[0]);
    if (core::dot(n_tri, sup(-n_tri)) - plane_d >= 0.0f) {
        return; // the body's lowest point along the normal is above the plane: no contact
    }

    // The triangle's side planes: through each edge, perpendicular to the triangle, facing out.
    const core::Vec3 centroid = (v[0] + v[1] + v[2]) * (1.0f / 3.0f);
    core::Vec3 side_n[3];
    float side_d[3];
    for (int e = 0; e < 3; ++e) {
        core::Vec3 sn = core::cross(v[(e + 1) % 3] - v[e], n_tri);
        if (core::dot(sn, centroid - v[e]) > 0.0f) {
            sn = -sn;
        }
        side_n[e] = sn;
        side_d[e] = core::dot(sn, v[e]);
    }

    const auto emit = [&](core::Vec3 p, std::uint32_t tag) {
        const float dist = core::dot(n_tri, p) - plane_d; // <= 0 below the surface
        if (dist > narrowphase_detail::kKeepEps) {
            return;
        }
        const float depth = std::max(0.0f, -dist);
        HeightfieldContact k;
        k.point = p + n_tri * (0.5f * depth);
        k.normal = n_tri;
        k.depth = depth;
        k.feature = feature_combine(feature_combine(seed, tri), tag);
        k.triangle = tri;
        push_unique(out, k);
    };

    // Clipping source.
    constexpr int kBuf = static_cast<int>(2 * hull_detail::kMaxHullFaceVertices);
    const std::uint32_t inc = narrowphase_detail::most_aligned_poly_face(poly, -n_tri);
    const std::uint32_t begin = poly.face_offsets[inc];
    int count = static_cast<int>(poly.face_offsets[inc + 1] - begin);
    ClipVertex buf_a[kBuf];
    ClipVertex buf_b[kBuf];
    for (int k = 0; k < count; ++k) {
        const std::uint32_t vi = poly.face_indices[begin + static_cast<std::uint32_t>(k)];
        buf_a[k] = ClipVertex{poly.pos + core::rotate(poly.orient, poly.verts[vi]), vi};
    }
    ClipVertex* cur = buf_a;
    ClipVertex* nxt = buf_b;
    for (int e = 0; e < 3 && count > 0; ++e) {
        count = narrowphase_detail::clip_against_plane(
            cur, count, side_n[e], side_d[e], static_cast<std::uint32_t>(e), nxt, kBuf);
        std::swap(cur, nxt);
    }
    for (int k = 0; k < count; ++k) {
        emit(cur[k].p, feature_combine(inc, cur[k].tag));
    }

    // Vertex-in-prism source.
    for (std::size_t i = 0; i < poly.verts.size(); ++i) {
        const core::Vec3 p = poly.pos + core::rotate(poly.orient, poly.verts[i]);
        bool inside = true;
        for (int e = 0; e < 3 && inside; ++e) {
            inside = core::dot(side_n[e], p) - side_d[e] <= 0.0f;
        }
        if (inside) {
            emit(p, feature_combine(0x7E27u, static_cast<std::uint32_t>(i)));
        }
    }
}

} // namespace heightfield_detail

// All contacts between one posed convex shape and the terrain, in the heightfield's LOCAL frame
// (`pos`/`q` already transformed into it). Cells come straight from the shape's bound — floor()
// of its min/max over the cell size, the grid being its own midphase — and are visited in
// ascending (row, column, triangle) order, so the candidate list (and everything built from it)
// is a pure function of the inputs. Returns the number appended.
inline std::size_t heightfield_contacts_local(const HeightfieldShape& hf,
                                              const ShapeDesc& s,
                                              core::Vec3 pos,
                                              const core::Quat& q,
                                              const ConvexHull* hull,
                                              std::uint32_t seed,
                                              std::vector<HeightfieldContact>& out) {
    using namespace heightfield_detail;
    const std::size_t before = out.size();
    const Aabb b = hull != nullptr ? hull_world_aabb(*hull, pos, q) : compute_aabb(s, pos, q);
    if (b.max.x < 0.0f || b.max.z < 0.0f || b.min.x > hf.extent_x() || b.min.z > hf.extent_z() ||
        b.min.y > hf.max_h || b.max.y < hf.min_h - hf.thickness) {
        return 0;
    }
    std::uint32_t i0 = 0, i1 = 0, j0 = 0, j1 = 0;
    cell_range(b.min.x, b.max.x, hf.dx, hf.cells_x(), i0, i1);
    cell_range(b.min.z, b.max.z, hf.dz, hf.cells_z(), j0, j1);

    // Pose the shape's pieces once.
    narrowphase_detail::BoxPolyStorage box_storage;
    narrowphase_detail::PolyView poly{};
    const bool is_poly = s.type == ShapeType::Box || s.type == ShapeType::ConvexHull;
    if (s.type == ShapeType::Box) {
        poly = narrowphase_detail::make_box_poly_view(box_storage, s, pos, q);
    } else if (s.type == ShapeType::ConvexHull) {
        if (hull == nullptr) {
            return 0;
        }
        poly = narrowphase_detail::make_hull_poly_view(*hull, pos, q);
    }
    const core::Vec3 cap_axis = core::rotate(q, core::Vec3{0.0f, s.half_height, 0.0f});

    for (std::uint32_t cj = j0; cj <= j1; ++cj) {
        for (std::uint32_t ci = i0; ci <= i1; ++ci) {
            const HeightfieldCell c = cell_of(hf, ci, cj);
            if (b.min.y > c.max_y()) {
                continue; // the whole shape is above this cell's highest corner
            }
            for (int half = 0; half < 2; ++half) {
                core::Vec3 v[3];
                std::uint32_t vid[3];
                triangle_vertices(hf, c, half, v, vid);
                const core::Vec3 n = c.normal(half);
                const std::uint32_t tri = triangle_id(hf, ci, cj, half);
                switch (s.type) {
                    case ShapeType::Sphere:
                        sphere_vs_triangle(pos, s.radius, v, vid, n, tri, seed, out);
                        break;
                    case ShapeType::Capsule:
                        capsule_vs_triangle(
                            pos - cap_axis, pos + cap_axis, s.radius, v, vid, n, tri, seed, out);
                        break;
                    default:
                        if (is_poly) {
                            poly_vs_triangle(poly, v, n, tri, seed, out);
                        }
                        break;
                }
            }
        }
    }
    return out.size() - before;
}

// Group candidate contacts into at most kMaxPatches manifold-sized patches by normal (see
// kPatchCos). Candidates arrive in ascending triangle order, so each patch's SEED — its first
// triangle, and its region id — is deterministic, and patches come out ordered by seed. A
// candidate that fits no patch when all four are taken joins the best-aligned one; the return value
// counts those folds (WorldStats::heightfield_patch_merges).
struct HeightfieldPatch {
    std::uint32_t seed_triangle = 0;
    core::Vec3 normal_sum{0.0f, 0.0f, 0.0f};
    core::Vec3 seed_normal{0.0f, 1.0f, 0.0f};
    std::vector<std::uint32_t> members; // indices into the candidate list
};

inline std::uint32_t group_patches(std::span<const HeightfieldContact> cands,
                                   std::vector<HeightfieldPatch>& patches) {
    using heightfield_detail::kMaxPatches;
    using heightfield_detail::kPatchCos;
    patches.clear();
    std::uint32_t merges = 0;
    for (std::uint32_t k = 0; k < cands.size(); ++k) {
        const HeightfieldContact& c = cands[k];
        std::size_t best = 0;
        float best_cos = -2.0f;
        for (std::size_t p = 0; p < patches.size(); ++p) {
            const float cs = core::dot(patches[p].seed_normal, c.normal);
            if (cs > best_cos) { // strict: the earliest patch wins a tie
                best_cos = cs;
                best = p;
            }
        }
        if (!patches.empty() && best_cos >= kPatchCos) {
            patches[best].members.push_back(k);
            patches[best].normal_sum += c.normal;
        } else if (patches.size() < kMaxPatches) {
            HeightfieldPatch np;
            // The region id is the seed triangle — made STRICTLY increasing across patches: seeds
            // are non-decreasing (candidates arrive in triangle order), but one triangle can seed
            // two patches (a capsule's end sphere on a face and its shaft on that face's edge have
            // different normals). A repeated id would make two regions one warm-start key and one
            // event, so the second takes the previous id + 1. Still ordered (the event merge needs
            // that), still stable while the contact configuration is.
            np.seed_triangle = patches.empty()
                                   ? c.triangle
                                   : std::max(c.triangle, patches.back().seed_triangle + 1u);
            np.seed_normal = c.normal;
            np.normal_sum = c.normal;
            np.members.push_back(k);
            patches.push_back(std::move(np));
        } else {
            patches[best].members.push_back(k);
            patches[best].normal_sum += c.normal;
            ++merges;
        }
    }
    return merges;
}

// Fill `m`'s normal and points from one patch (terrain→body normal; the caller flips it if the
// terrain is body b). More than four points are reduced by the convex path's own reduce_manifold
// (deepest, farthest, then maximum spread), so a box on terrain gets the same four-corner patch it
// gets on a box floor.
inline void patch_to_manifold(const HeightfieldPatch& patch,
                              std::span<const HeightfieldContact> cands,
                              core::Vec3 pos,
                              const core::Quat& q,
                              Manifold& m) {
    using narrowphase_detail::ClipVertex;
    m.count = 0;
    const float len = core::length(patch.normal_sum);
    const core::Vec3 n_local =
        len > narrowphase_detail::kNormalEps ? patch.normal_sum * (1.0f / len) : patch.seed_normal;
    m.normal = core::rotate(q, n_local);
    m.patch = patch.seed_triangle;
    const std::size_t count = patch.members.size();
    if (count <= 4) {
        for (const std::uint32_t k : patch.members) {
            narrowphase_detail::add_point(
                m, pos + core::rotate(q, cands[k].point), cands[k].depth, cands[k].feature);
        }
        return;
    }
    std::vector<ClipVertex> verts(count);
    std::vector<float> pens(count);
    std::vector<float> zeros(count, 0.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const HeightfieldContact& c = cands[patch.members[i]];
        verts[i] = ClipVertex{pos + core::rotate(q, c.point), c.feature};
        pens[i] = c.depth;
    }
    narrowphase_detail::reduce_manifold(
        m, verts.data(), pens.data(), static_cast<int>(count), m.normal, zeros.data());
}

// Does a sphere (local frame) touch the terrain? The overlap_sphere exact test: any triangle within
// `r` of the centre from above, or the centre sunk beneath a triangle it lies over.
[[nodiscard]] inline bool
sphere_overlaps_heightfield_local(const HeightfieldShape& hf, core::Vec3 c, float r) {
    ShapeDesc s;
    s.type = ShapeType::Sphere;
    s.radius = r;
    std::vector<HeightfieldContact> scratch;
    return heightfield_contacts_local(hf, s, c, core::quat_identity(), nullptr, 0u, scratch) > 0;
}

// ─── Convex queries against terrain (m19.2) ──────────────────────────────────────────────────
//
// M19.1 shipped the ray walk and the contact build, and deferred the three places a CONVEX shape
// meets terrain: `shape_cast`, `penetration`, and speculative CCD. All three skipped terrain and
// counted the skip. This closes them, and the shape of the solution is the same in all three
// because a heightfield cell's two triangles are each CONVEX: the engine's existing GJK, EPA and
// conservative-advancement cast already handle any convex pair through a support function, and
// `narrowphase_detail::PolySupport` over a three-vertex `PolyView` IS a triangle's support
// function. So terrain needs no new geometric kernel — only an enumeration of which triangles to
// hand the kernels, and a rule for combining the per-triangle answers.
//
// WHY PER-TRIANGLE RATHER THAN A SUPPORT FUNCTION FOR THE WHOLE FIELD. A heightfield is not
// convex, so it has no support function (support.hpp returns the origin for it and says so). The
// alternative — the one physics engines that "support heightfields in GJK" actually implement — is
// to pick a local convex piece and pretend; that silently gives wrong answers near a ridge, where
// the nearest triangle is not the one the shape is about to hit. Enumerating the candidates and
// taking the best answer per kernel is slower and right.
//
// THE CANDIDATE SET IS THE SHAPE'S BOUND OVER THE GRID, exactly as the contact build's is, via the
// shared `cell_range` — one definition of "which cells does this bound touch", so a query can
// never enumerate a different set from the narrowphase that will have to resolve what it found.
// For a cast the bound is the SWEPT one (the caster's AABB at t = 0 unioned with its AABB at
// t = tmax), which is conservative: a convex shape swept along a straight line stays inside the
// union of its end poses' boxes, because each coordinate is a linear function of t.
//
// THE BUDGET IS EXPLICIT AND COUNTED. A long cast across a large tile can sweep a bound covering
// more cells than it is worth testing one at a time — a 500 m cast over a 0.37 m grid is ~1.8 M
// triangles. Rather than stall a frame, the enumeration stops at `kMaxQueryTriangles` and reports
// that it was truncated, so the caller bumps the same `heightfield_query_skips()` counter M19.1
// used: "the query saw nothing" and "the query could not look at all of it" stay distinguishable,
// which is the whole reason that counter exists. A character controller — the motivating caller,
// and the one ADR-0060 named — sweeps a metre or two and touches tens of triangles, nowhere near
// the cap. Deferred, and named in the ADR: walking the sweep with the grid DDA and testing only a
// band of cells around each step, which is O(cells crossed) instead of O(cells in the bound) and
// would retire the cap rather than raise it.

namespace heightfield_detail {

// Candidate ceiling for one convex query (see above). 8192 triangles = 4096 cells, i.e. a 64x64
// cell patch — far more than any character-scale sweep touches, and small enough that hitting it
// costs microseconds rather than a frame.
inline constexpr std::size_t kMaxQueryTriangles = 8192;

// One terrain triangle posed as a support function, with storage for its three vertices. The
// vertices are in the HEIGHTFIELD'S LOCAL frame and the view is posed with identity, because every
// routine below has already transformed the query into that frame — so GJK/EPA/the cast all run in
// local space and only the final answer is rotated back out. Keeping the pose out of the inner loop
// is also what makes the support function a pure vertex argmax.
struct TriangleSupportStorage {
    core::Vec3 verts[3];
    core::Vec3 normal;
    std::uint32_t vid[3];
    std::uint32_t tri;
    narrowphase_detail::PolyView view;

    void set(const HeightfieldShape& hf, const HeightfieldCell& c, int half) noexcept {
        triangle_vertices(hf, c, half, verts, vid);
        normal = c.normal(half);
        tri = triangle_id(hf, c.ci, c.cj, half);
        view.verts = std::span<const core::Vec3>(verts, 3);
        view.face_normals = {};
        view.face_offsets = {};
        view.face_indices = {};
        view.pos = core::Vec3{};
        view.orient = core::quat_identity();
    }

    [[nodiscard]] narrowphase_detail::PolySupport support() const noexcept {
        return narrowphase_detail::PolySupport{&view};
    }

    // The triangle's centroid — the "centre" the kernels want for their initial search direction
    // and for the speculative rule's relative-velocity frame. A triangle has no stored centre, and
    // using a vertex instead would bias the first GJK direction toward one corner.
    [[nodiscard]] core::Vec3 centroid() const noexcept {
        return (verts[0] + verts[1] + verts[2]) * (1.0f / 3.0f);
    }
};

// Visit every candidate triangle for an AABB over the grid, in ascending (row, column, half)
// order — the contact build's order, so two routines looking at the same bound agree on which
// triangle they saw first and every tie-break below is a pure function of the inputs. Returns
// false when the bound was truncated by the budget (the caller counts that).
template <typename Fn>
[[nodiscard]] inline bool
for_each_candidate_triangle(const HeightfieldShape& hf, const Aabb& bound, Fn&& fn) {
    // Entirely outside the tile (including below its solid band) ⇒ no candidates, not truncated.
    if (bound.max.x < 0.0f || bound.max.z < 0.0f || bound.min.x > hf.extent_x() ||
        bound.min.z > hf.extent_z() || bound.min.y > hf.max_h ||
        bound.max.y < hf.min_h - hf.thickness) {
        return true;
    }
    std::uint32_t i0 = 0, i1 = 0, j0 = 0, j1 = 0;
    cell_range(bound.min.x, bound.max.x, hf.dx, hf.cells_x(), i0, i1);
    cell_range(bound.min.z, bound.max.z, hf.dz, hf.cells_z(), j0, j1);

    std::size_t tested = 0;
    TriangleSupportStorage tri;
    for (std::uint32_t cj = j0; cj <= j1; ++cj) {
        for (std::uint32_t ci = i0; ci <= i1; ++ci) {
            const HeightfieldCell c = cell_of(hf, ci, cj);
            for (int half = 0; half < 2; ++half) {
                if (tested >= kMaxQueryTriangles) {
                    return false; // truncated — the caller counts it
                }
                ++tested;
                tri.set(hf, c, half);
                fn(tri);
            }
        }
    }
    return true;
}

} // namespace heightfield_detail

// ── shape_cast against terrain ──
//
// Sweep one posed convex shape along `dir` (unit) for at most `tmax` and report the first terrain
// triangle it reaches. Everything is in the heightfield's LOCAL frame. `truncated` is set when the
// budget cut the candidate set short, so the caller can count a partial look as a skip rather than
// as a clean miss. Returns false with `overlap_out` true when the shape ALREADY overlaps terrain at
// t = 0 — the same contract `shape_cast` has for convex targets, where the caller is told to run
// `penetration` instead, because a cast has no time of impact to report for a shape that starts
// inside something.
[[nodiscard]] inline bool cast_shape_vs_heightfield_local(const HeightfieldShape& hf,
                                                          const ShapeDesc& s,
                                                          core::Vec3 origin,
                                                          const core::Quat& q,
                                                          const ConvexHull* hull,
                                                          core::Vec3 dir,
                                                          float tmax,
                                                          float& t_out,
                                                          core::Vec3& n_out,
                                                          core::Vec3& p_out,
                                                          bool& overlap_out,
                                                          bool& truncated) {
    using namespace heightfield_detail;
    overlap_out = false;
    truncated = false;

    // The swept bound: the caster's box at both ends of the sweep, unioned. Conservative because
    // each coordinate of a linearly swept convex shape is linear in t, so the shape never leaves
    // the union of its endpoint boxes.
    const Aabb a0 =
        hull != nullptr ? hull_world_aabb(*hull, origin, q) : compute_aabb(s, origin, q);
    const core::Vec3 end = origin + dir * tmax;
    const Aabb a1 = hull != nullptr ? hull_world_aabb(*hull, end, q) : compute_aabb(s, end, q);
    const Aabb swept{core::Vec3{std::min(a0.min.x, a1.min.x),
                                std::min(a0.min.y, a1.min.y),
                                std::min(a0.min.z, a1.min.z)},
                     core::Vec3{std::max(a0.max.x, a1.max.x),
                                std::max(a0.max.y, a1.max.y),
                                std::max(a0.max.z, a1.max.z)}};

    bool hit = false;
    float best_t = tmax;
    std::uint32_t best_tri = 0;
    core::Vec3 best_n{0.0f, 1.0f, 0.0f};
    core::Vec3 best_p{};
    bool overlapped = false;

    const bool complete =
        for_each_candidate_triangle(hf, swept, [&](const TriangleSupportStorage& tri) {
            if (overlapped) {
                return; // an initial overlap outranks any time of impact; stop refining
            }
            const narrowphase_detail::PolySupport target = tri.support();
            float t = 0.0f;
            core::Vec3 n{};
            core::Vec3 p{};
            bool overlap = false;
            if (!cast_convex_vs_convex(
                    s, origin, q, hull, target, tri.centroid(), dir, tmax, t, n, p, overlap)) {
                return;
            }
            if (overlap) {
                // An initial overlap outranks any time of impact (the convex path's rule), and it
                // still reports a normal and a point: the caller is about to run `penetration`,
                // and handing it an uninitialised axis would be worse than handing it this
                // triangle's. Recorded from the FIRST overlapping triangle in grid order, so it is
                // a pure function of the inputs like every other answer here.
                overlapped = true;
                best_n = n;
                best_p = p;
                return;
            }
            // Earliest wins, with an exact-tie break toward the LOWER TRIANGLE ID so the answer
            // does not depend on enumeration order being stable for a reason other than the grid.
            // A sweep that reaches a shared edge touches two triangles at the identical t, which is
            // the common case on terrain rather than a corner one.
            if (!hit || t < best_t || (t == best_t && tri.tri < best_tri)) {
                hit = true;
                best_t = t;
                best_tri = tri.tri;
                best_n = n;
                best_p = p;
            }
        });
    if (!complete) {
        truncated = true;
    }
    if (overlapped) {
        overlap_out = true;
        n_out = best_n;
        p_out = best_p;
        return false;
    }
    if (!hit) {
        return false;
    }
    t_out = best_t;
    n_out = best_n;
    p_out = best_p;
    return true;
}

// ── penetration against terrain ──
//
// Deepest overlap between one posed convex shape and the terrain, in the heightfield's LOCAL
// frame. `normal` points the way the QUERY SHAPE must move to separate, and `depth` is how far.
//
// THIS DELIBERATELY DOES NOT RUN EPA PER TRIANGLE, and the reason is the whole subtlety of the
// brick. EPA answers "what is the shortest translation that separates these two convex shapes",
// and a cell triangle is a ZERO-THICKNESS surface — so for a shape that has sunk into the ground,
// the shortest separation is very often DEEPER INTO IT. Measured while building this: a sphere of
// radius 0.5 whose centre sits 0.1 below a flat tile spans [-0.4, +0.6] about the surface, and EPA
// correctly reported that pushing it 0.4 DOWN separates it from the triangle sooner than pushing
// it 0.6 up. That is the right answer about a triangle and a catastrophic answer about ground: a
// character controller handed it would depenetrate itself into the rock. The sideways case is the
// same bug — a shape over a shared edge can leave a triangle's extent more cheaply than it can
// leave the surface.
//
// The contact build already owns the correct rule, because the narrowphase had to solve exactly
// this: `sphere_vs_triangle` and friends only accept a below-plane overlap when the shape lies
// over the triangle's INTERIOR, and then resolve it along the triangle's own normal — "straight
// back up, never sideways". So this query reuses `heightfield_contacts_local` and takes its
// deepest contact. One rule for what "inside the ground" means, shared by the solver and by the
// query that tells a caller how to get out of it; a second derivation here would be a second
// chance to disagree with the thing that actually moves bodies.
//
// Deepest, not nearest, matching the convex query: repeatedly resolving the worst violation
// strictly reduces the maximum, so a shape wedged in a crevice converges out of it.
[[nodiscard]] inline bool penetration_vs_heightfield_local(const HeightfieldShape& hf,
                                                           const ShapeDesc& s,
                                                           core::Vec3 pos,
                                                           const core::Quat& q,
                                                           const ConvexHull* hull,
                                                           float& depth_out,
                                                           core::Vec3& normal_out,
                                                           core::Vec3& point_out,
                                                           bool& truncated) {
    // The contact build enumerates from the shape's own bound and has no budget to exceed: its
    // candidate set is the bound's cells, which is what this query would have used anyway. So
    // there is no truncation to report here — the flag stays in the signature because the caller
    // treats all three terrain queries uniformly, and a later banded enumeration may reintroduce
    // one.
    truncated = false;
    std::vector<HeightfieldContact> cands;
    if (heightfield_contacts_local(hf, s, pos, q, hull, 0u, cands) == 0) {
        return false;
    }
    bool found = false;
    float best_depth = 0.0f;
    std::uint32_t best_tri = 0;
    core::Vec3 best_normal{0.0f, 1.0f, 0.0f};
    core::Vec3 best_point{};
    for (const HeightfieldContact& c : cands) {
        // Zero depth is a TOUCH, not a penetration — the same filter the convex query applies to
        // EPA's depth, and for the same reason: there is nothing to push out of.
        if (!(c.depth > 0.0f)) {
            continue;
        }
        if (!found || c.depth > best_depth || (c.depth == best_depth && c.triangle < best_tri)) {
            found = true;
            best_depth = c.depth;
            best_tri = c.triangle;
            best_normal = c.normal;
            best_point = c.point;
        }
    }
    if (!found) {
        return false;
    }
    depth_out = best_depth;
    // HeightfieldContact::normal is terrain → shape (the convention the manifolds carry), which is
    // already the direction the SHAPE must move to separate. No negation: the sign is inherited
    // from the contact build rather than re-reasoned.
    normal_out = best_normal;
    point_out = best_point;
    return true;
}

// ── speculative CCD against terrain ──
//
// The nearest imminent contact between one posed convex shape moving at `v` and the terrain, in
// the heightfield's LOCAL frame, as a one-point manifold carrying a NEGATIVE penetration (the gap
// still to close). Terrain is static, so its velocity is zero. The rule is not re-derived here:
// `collide_speculative_supports` owns it, and this routine only enumerates triangles and keeps the
// nearest gap — so a body approaching terrain is arrested by exactly the same condition that
// arrests it against a wall, including the closing-speed test and the slop.
//
// NEAREST, NOT DEEPEST: for a speculative contact the smallest gap is the one that will be touched
// first, and letting a farther triangle win would let the solver permit motion through the nearer
// one.
//
// THE NORMAL IS TERRAIN → SHAPE, which is `patch_to_manifold`'s convention and NOT what
// `collide_speculative_supports` hands back. That function returns a → b for the order it was
// called in, and it is called here as (shape, triangle) — so its answer is shape → terrain and is
// negated once, below, before it leaves. Matching the exact path's convention is the point: the
// caller then applies the identical "flip it if the terrain is body b" rule to both, instead of
// two routines with two conventions and one chance to get a sign backwards. (It WAS backwards:
// the first version of this brick negated on the wrong branch, and the fast-projectile test
// measured the body sailing through the ground exactly as it had before CCD was wired at all.)
[[nodiscard]] inline bool speculative_vs_heightfield_local(const HeightfieldShape& hf,
                                                           const ShapeDesc& s,
                                                           core::Vec3 pos,
                                                           const core::Quat& q,
                                                           const ConvexHull* hull,
                                                           core::Vec3 v,
                                                           float dt,
                                                           Manifold& m,
                                                           bool& truncated) {
    using namespace heightfield_detail;
    truncated = false;
    m.count = 0;
    if (!(dt > 0.0f)) {
        return false;
    }
    // The bound is the SWEPT one for the same reason the cast's is: a contact that is imminent is
    // by definition one the shape's current box may not yet reach.
    const Aabb a0 = hull != nullptr ? hull_world_aabb(*hull, pos, q) : compute_aabb(s, pos, q);
    const core::Vec3 end = pos + v * dt;
    const Aabb a1 = hull != nullptr ? hull_world_aabb(*hull, end, q) : compute_aabb(s, end, q);
    const Aabb swept{core::Vec3{std::min(a0.min.x, a1.min.x),
                                std::min(a0.min.y, a1.min.y),
                                std::min(a0.min.z, a1.min.z)},
                     core::Vec3{std::max(a0.max.x, a1.max.x),
                                std::max(a0.max.y, a1.max.y),
                                std::max(a0.max.z, a1.max.z)}};
    const ShapeSupport query{&s, pos, q, hull};

    bool found = false;
    float best_gap = 0.0f;
    std::uint32_t best_tri = 0;
    Manifold best{};

    const bool complete =
        for_each_candidate_triangle(hf, swept, [&](const TriangleSupportStorage& tri) {
            Manifold cand;
            cand.count = 0;
            if (!collide_speculative_supports(
                    query, pos, v, tri.support(), tri.centroid(), core::Vec3{}, dt, cand)) {
                return;
            }
            // Into the terrain → shape convention (see the header comment) before any comparison,
            // so what is stored is what is returned.
            cand.normal = cand.normal * -1.0f;
            // `penetration` is the NEGATIVE gap here, so the nearest triangle is the one whose
            // penetration is LARGEST (closest to zero).
            const float gap = cand.points[0].penetration;
            if (!found || gap > best_gap || (gap == best_gap && tri.tri < best_tri)) {
                found = true;
                best_gap = gap;
                best_tri = tri.tri;
                best = cand;
            }
        });
    if (!complete) {
        truncated = true;
    }
    if (!found) {
        return false;
    }
    m = best;
    return true;
}

} // namespace rime::physics
