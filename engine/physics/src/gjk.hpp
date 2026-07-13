// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>

#include "rime/core/math/vec.hpp"

// GJK (Gilbert–Johnson–Keerthi) — the exact convex overlap/distance test the narrowphase runs on
// every broadphase candidate that has no cheaper closed form (full derivation with pictures:
// docs/math/gjk-epa.md).
//
// The reformulation that makes it work: two convex sets A and B overlap exactly when their
// Minkowski DIFFERENCE  M = A - B = { a - b }  contains the origin, and their distance is the
// distance from the origin to M. GJK never builds M (it can have millions of faces); it only
// samples it through the support function  support_M(d) = support_A(d) - support_B(-d)  and keeps
// a SIMPLEX (1–4 of those support points) that it walks toward the origin:
//
//   1. find the point of the current simplex closest to the origin;
//   2. reduce the simplex to the minimal feature (vertex/edge/face) carrying that point;
//   3. grab a new support point in the direction from that point toward the origin;
//   4. if the new point gets no closer, the origin is outside M -> SEPARATED, distance known;
//      if the simplex becomes a tetrahedron enclosing the origin -> OVERLAPPING.
//
// Step 1–2 (the "distance subalgorithm") is the heart: closest-point-on-simplex via barycentric
// case analysis (the Ericson/Real-Time-Collision-Detection formulation, specialized to the query
// point being the origin). We track, for every Minkowski vertex, the two shape points it came
// from — so when GJK converges separated, the same barycentric weights reconstruct the closest
// points ON A AND ON B (the witnesses the capsule fast paths and, at M7.10, speculative contacts
// consume). When it detects overlap, the terminal simplex seeds EPA (src/epa.hpp).
//
// Private header (under src/), same discipline as aabb_tree.hpp: invisible above the seam.
namespace rime::physics {

// One Minkowski-difference vertex plus the two shape-space support points that formed it.
struct SupportVertex {
    core::Vec3 w; // support_A(d) - support_B(-d): the point of M we actually test
    core::Vec3 a; // support_A(d)   (witness on A)
    core::Vec3 b; // support_B(-d)  (witness on B)
};

namespace gjk_detail {

// Tolerances. The engine simulates at metre scale (bodies ~0.05–10 m), so absolute epsilons are
// chosen against that: kTouchEps2 treats the origin as ON the simplex below ~1e-5 m (a contact of
// depth ~0), kRelEps stops iterating when the support point improves the squared distance by less
// than 0.01% (float has ~7 digits; pushing further just spins on noise). Calibration points, not
// truths — revisited with the M7.10 stress harness.
inline constexpr float kTouchEps2 = 1e-10f;
inline constexpr float kRelEps = 1e-4f;
inline constexpr float kDuplicateEps2 = 1e-12f;
inline constexpr float kTinyVol = 1e-9f;
inline constexpr int kMaxIterations = 32;

// Result of a closest-point query against one simplex feature: the point itself, and the
// barycentric weights over the vertices that actually support it (the reduced simplex).
struct FeatureClosest {
    core::Vec3 point;
    float lambda[3];
    int index[3]; // indices into the caller's vertex array
    int count = 0;
    float dist2 = 0.0f;
};

// Closest point to the ORIGIN on segment v[i0]v[i1], with barycentrics. The unclamped parameter
// t = -a.(b-a)/|b-a|^2 is clamped to the segment; t landing strictly inside keeps both vertices,
// an endpoint reduces the simplex to that vertex alone.
[[nodiscard]] inline FeatureClosest
closest_on_segment(const SupportVertex* v, int i0, int i1) noexcept {
    const core::Vec3 a = v[i0].w;
    const core::Vec3 b = v[i1].w;
    const core::Vec3 ab = b - a;
    const float len2 = core::dot(ab, ab);
    FeatureClosest r;
    if (len2 <= kDuplicateEps2) { // degenerate segment: both vertices coincide
        r.point = a;
        r.lambda[0] = 1.0f;
        r.index[0] = i0;
        r.count = 1;
        r.dist2 = core::dot(a, a);
        return r;
    }
    const float t = -core::dot(a, ab) / len2;
    if (t <= 0.0f) {
        r.point = a;
        r.lambda[0] = 1.0f;
        r.index[0] = i0;
        r.count = 1;
    } else if (t >= 1.0f) {
        r.point = b;
        r.lambda[0] = 1.0f;
        r.index[0] = i1;
        r.count = 1;
    } else {
        r.point = a + ab * t;
        r.lambda[0] = 1.0f - t;
        r.lambda[1] = t;
        r.index[0] = i0;
        r.index[1] = i1;
        r.count = 2;
    }
    r.dist2 = core::dot(r.point, r.point);
    return r;
}

// Closest point to the ORIGIN on triangle v[i0]v[i1]v[i2]: the classic seven-region Voronoi case
// analysis (three vertex regions, three edge regions, the face region), each region giving exact
// barycentrics. The region tests are arranged so exactly one fires; a degenerate (collinear)
// triangle would blow up the face-region division, so it falls back to the best edge.
[[nodiscard]] inline FeatureClosest
closest_on_triangle(const SupportVertex* v, int i0, int i1, int i2) noexcept {
    const core::Vec3 a = v[i0].w;
    const core::Vec3 b = v[i1].w;
    const core::Vec3 c = v[i2].w;
    const core::Vec3 ab = b - a;
    const core::Vec3 ac = c - a;

    FeatureClosest r;
    const auto vertex_case = [&](int idx, core::Vec3 p) {
        r.point = p;
        r.lambda[0] = 1.0f;
        r.index[0] = idx;
        r.count = 1;
        r.dist2 = core::dot(p, p);
    };
    const auto edge_case = [&](int ia, int ib, core::Vec3 pa, core::Vec3 e, float t) {
        r.point = pa + e * t;
        r.lambda[0] = 1.0f - t;
        r.lambda[1] = t;
        r.index[0] = ia;
        r.index[1] = ib;
        r.count = 2;
        r.dist2 = core::dot(r.point, r.point);
    };

    // Region of vertex a: the origin lies "behind" a relative to both edges.
    const core::Vec3 ap = -a;
    const float d1 = core::dot(ab, ap);
    const float d2 = core::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        vertex_case(i0, a);
        return r;
    }

    const core::Vec3 bp = -b;
    const float d3 = core::dot(ab, bp);
    const float d4 = core::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        vertex_case(i1, b);
        return r;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        edge_case(i0, i1, a, ab, denom != 0.0f ? d1 / denom : 0.0f);
        return r;
    }

    const core::Vec3 cp = -c;
    const float d5 = core::dot(ab, cp);
    const float d6 = core::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        vertex_case(i2, c);
        return r;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        edge_case(i0, i2, a, ac, denom != 0.0f ? d2 / denom : 0.0f);
        return r;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float denom = (d4 - d3) + (d5 - d6);
        edge_case(i1, i2, b, c - b, denom != 0.0f ? (d4 - d3) / denom : 0.0f);
        return r;
    }

    // Face region. va+vb+vc is proportional to the triangle's squared area; if it has collapsed
    // (collinear vertices) no face exists — take the best of the three edges instead.
    const float sum = va + vb + vc;
    if (std::fabs(sum) <= kTinyVol) {
        FeatureClosest best = closest_on_segment(v, i0, i1);
        const FeatureClosest e1 = closest_on_segment(v, i0, i2);
        if (e1.dist2 < best.dist2) {
            best = e1;
        }
        const FeatureClosest e2 = closest_on_segment(v, i1, i2);
        if (e2.dist2 < best.dist2) {
            best = e2;
        }
        return best;
    }
    const float denom = 1.0f / sum;
    const float bv = vb * denom;
    const float bw = vc * denom;
    r.point = a + ab * bv + ac * bw;
    r.lambda[0] = 1.0f - bv - bw;
    r.lambda[1] = bv;
    r.lambda[2] = bw;
    r.index[0] = i0;
    r.index[1] = i1;
    r.index[2] = i2;
    r.count = 3;
    r.dist2 = core::dot(r.point, r.point);
    return r;
}

} // namespace gjk_detail

// Solve the current simplex: find the point closest to the origin, REDUCE the simplex (in place,
// witnesses included) to the minimal feature supporting that point, and report containment when a
// non-degenerate tetrahedron encloses the origin. `lambda` returns the barycentric weights of the
// surviving vertices (used to reconstruct witness points).
[[nodiscard]] inline bool
solve_simplex(SupportVertex* verts, float* lambda, int& count, core::Vec3& closest) noexcept {
    using namespace gjk_detail;

    FeatureClosest best;
    bool contained = false;

    switch (count) {
        case 1:
            best.point = verts[0].w;
            best.lambda[0] = 1.0f;
            best.index[0] = 0;
            best.count = 1;
            break;
        case 2:
            best = closest_on_segment(verts, 0, 1);
            break;
        case 3:
            best = closest_on_triangle(verts, 0, 1, 2);
            break;
        default: {
            // Tetrahedron: the origin is inside iff it is on the interior side of all four face
            // planes (each oriented by the opposite vertex). Only faces the origin is OUTSIDE of
            // can carry the closest point, so we evaluate exactly those. A degenerate (flat)
            // tetrahedron has no reliable planes — evaluate every face and never claim
            // containment; GJK then keeps iterating from the reduced feature.
            static constexpr int kFaces[4][4] = {
                {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 3, 1}, {1, 2, 3, 0}};
            const core::Vec3 e1 = verts[1].w - verts[0].w;
            const core::Vec3 e2 = verts[2].w - verts[0].w;
            const core::Vec3 e3 = verts[3].w - verts[0].w;
            const bool degenerate = std::fabs(core::dot(core::cross(e1, e2), e3)) <= kTinyVol;

            bool evaluated = false;
            best.dist2 = 0.0f;
            for (const auto& f : kFaces) {
                const core::Vec3 fa = verts[f[0]].w;
                const core::Vec3 n = core::cross(verts[f[1]].w - fa, verts[f[2]].w - fa);
                const float side_origin = core::dot(n, -fa);
                const float side_opp = core::dot(n, verts[f[3]].w - fa);
                const bool outside =
                    degenerate || std::fabs(side_opp) <= kTinyVol || side_origin * side_opp < 0.0f;
                if (!outside) {
                    continue;
                }
                const FeatureClosest fc = closest_on_triangle(verts, f[0], f[1], f[2]);
                if (!evaluated || fc.dist2 < best.dist2) {
                    best = fc;
                    evaluated = true;
                }
            }
            if (!evaluated) {
                contained = true; // interior side of every face: the origin is enclosed
            }
            break;
        }
    }

    if (contained) {
        closest = core::Vec3{};
        return true;
    }

    // Compact the simplex to the supporting feature. Copy out first: `best.index` refers to the
    // ORIGINAL vertex order, and writing verts[0] before reading verts[index[1]] would corrupt it.
    SupportVertex kept[3];
    for (int i = 0; i < best.count; ++i) {
        kept[i] = verts[best.index[i]];
    }
    for (int i = 0; i < best.count; ++i) {
        verts[i] = kept[i];
        lambda[i] = best.lambda[i];
    }
    count = best.count;
    closest = best.point;
    return false;
}

// The GJK query result. Exactly one of the two outcomes:
//  - overlapping: the shapes intersect; `simplex` (up to 4 vertices) seeds EPA.
//  - separated:  `distance` > 0 with `point_a`/`point_b` the closest points on each shape.
struct GjkResult {
    bool overlapping = false;
    float distance = 0.0f;
    core::Vec3 point_a{};
    core::Vec3 point_b{};
    SupportVertex simplex[4];
    int simplex_count = 0;
};

// Run GJK over two posed convex shapes given as support callables (src/support.hpp). `seed_dir`
// is only a starting guess (typically centre_A - centre_B); any non-degenerate value converges,
// a good one converges in 2–4 iterations.
template <class SupA, class SupB>
[[nodiscard]] GjkResult gjk(const SupA& support_a, const SupB& support_b, core::Vec3 seed_dir) {
    using namespace gjk_detail;

    const auto minkowski_support = [&](core::Vec3 d) -> SupportVertex {
        SupportVertex sv;
        sv.a = support_a(d);
        sv.b = support_b(-d);
        sv.w = sv.a - sv.b;
        return sv;
    };

    GjkResult res;
    if (core::dot(seed_dir, seed_dir) <= kDuplicateEps2) {
        seed_dir = core::Vec3{1.0f, 0.0f, 0.0f};
    }

    SupportVertex verts[4];
    float lambda[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    int count = 1;
    verts[0] = minkowski_support(seed_dir);
    core::Vec3 closest = verts[0].w;

    const auto finish_separated = [&] {
        res.overlapping = false;
        res.point_a = core::Vec3{};
        res.point_b = core::Vec3{};
        for (int i = 0; i < count; ++i) {
            res.point_a += verts[i].a * lambda[i];
            res.point_b += verts[i].b * lambda[i];
        }
        res.distance = core::length(closest);
        for (int i = 0; i < count; ++i) {
            res.simplex[i] = verts[i];
        }
        res.simplex_count = count;
    };
    const auto finish_overlapping = [&] {
        res.overlapping = true;
        for (int i = 0; i < count; ++i) {
            res.simplex[i] = verts[i];
        }
        res.simplex_count = count;
    };

    float dist2 = core::dot(closest, closest);
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        // Origin (numerically) on the simplex: a touching/overlapping contact. EPA sorts out the
        // real depth — the simplex here may be a chord through a deeply overlapping difference.
        if (dist2 <= kTouchEps2) {
            finish_overlapping();
            return res;
        }

        const core::Vec3 d = -closest;
        const SupportVertex w = minkowski_support(d);

        // Convergence bound: the support plane through w perpendicular to `closest` bounds M, so
        // if w is no closer to the origin than `closest` (up to a relative tolerance), no point
        // of M is — the origin is outside and `closest` is (within tolerance) the answer. When
        // the origin is INSIDE M this test can never fire: every support along -closest passes
        // the origin (dot(closest, w) <= 0), keeping the left side >= dist2.
        if (dist2 - core::dot(closest, w.w) <= kRelEps * dist2) {
            finish_separated();
            return res;
        }

        // Cycling guard: re-encountering a vertex means fp noise is driving the loop, not
        // geometry. Accept the current answer as separated (distance is a valid upper bound).
        bool duplicate = false;
        for (int i = 0; i < count; ++i) {
            const core::Vec3 diff = w.w - verts[i].w;
            if (core::dot(diff, diff) <= kDuplicateEps2) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            finish_separated();
            return res;
        }

        verts[count] = w;
        ++count;

        if (solve_simplex(verts, lambda, count, closest)) {
            finish_overlapping();
            return res;
        }

        const float new_dist2 = core::dot(closest, closest);
        // Monotonicity guard: exact GJK strictly descends; if float arithmetic stopped making
        // progress, stop with the current (upper-bound) answer instead of spinning.
        if (new_dist2 >= dist2) {
            finish_separated();
            return res;
        }
        dist2 = new_dist2;
    }

    // Iteration cap (never hit in practice at our scales; a safety net, not a code path we rely
    // on). Report the current state honestly: near-zero distance as overlap, else separated.
    if (dist2 <= kTouchEps2) {
        finish_overlapping();
    } else {
        finish_separated();
    }
    return res;
}

} // namespace rime::physics
