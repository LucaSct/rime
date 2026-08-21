// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
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
//
// kTouchEps2 is a FAST PATH, not the touch discriminator: at large coordinates a genuinely
// touching pose can stall with dist2 well above it, which is why the stall exits demand a
// separation CERTIFICATE (see finish_stalled in gjk()) instead of trusting whatever distance the
// stalled simplex happens to hold. Its absoluteness — the disease #131 cured twice in this header
// — is harmless in a fast path that only ever short-circuits toward "overlapping".
inline constexpr float kTouchEps2 = 1e-10f;
inline constexpr float kRelEps = 1e-4f;
// The absolute noise floor of the convergence test, as a multiple of |closest| * |w| — the scale of
// the products inside it. A few float epsilons: large enough to cover the rounding of that dot
// product, small enough that it never stops a comparison the arithmetic could still resolve.
inline constexpr float kSupportEps = 4e-7f;
inline constexpr float kDuplicateEps2 = 1e-12f;
inline constexpr float kTinyVol = 1e-9f;
// Relative companion to kTinyVol, as a fraction of |edge|^2 * |point|^2 — the scale of the products
// va/vb/vc are differenced from. Sized by measurement, not taste: on a real degenerate triangle
// those three came out as exactly 1-2 ULP of a ~16.1 product, summing to 4 ULP, so a coefficient
// of 3.4 ULP sat just under it and the check did not fire. This is ~16 ULP.
//
// Being generous is the SAFE direction here, which is why it is not tuned to the edge. Falling
// back to the best edge is never wrong — an edge of the simplex is a subset of it, so its closest
// point remains a valid upper bound and GJK's convergence argument is untouched. It is only
// potentially less tight. Whereas accepting a sliver as a real face divides by noise and returns
// its CENTROID, which is not an approximation of anything.
inline constexpr float kDegenerateRel = 2e-6f;
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
    //
    // THE THRESHOLD MUST SCALE WITH THE TRIANGLE, and an absolute one is why it did not fire when
    // it mattered. va/vb/vc are built from dot products of edge vectors with point vectors, so
    // their float error grows as |edge|^2 * |point|^2 — for vertices a metre out that is ~1e-7,
    // a hundred times the fixed 1e-9 epsilon. A collinear triangle whose `sum` should be zero
    // therefore reads as 1e-7, the check passes it as a real face, and the division below turns
    // pure noise into barycentric weights: the answer comes back as the triangle's CENTROID.
    //
    // Measured, sphere vs a 1 m box at a 1.9e-5 m gap: GJK returned distance 0.47 for a true gap
    // of 1.9e-5, with a closest point at (0, -0.33, -0.33) — the centroid of a sliver whose three
    // vertices were collinear along the box's y=z diagonal. That is where m12.1's diagonal contact
    // normal came from, and why it looked like an edge direction: it was one.
    const float sum = va + vb + vc;
    const float edge2 = std::max(core::dot(ab, ab), core::dot(ac, ac));
    const float point2 = std::max(core::dot(a, a), std::max(core::dot(b, b), core::dot(c, c)));
    if (std::fabs(sum) <= kTinyVol + kDegenerateRel * edge2 * point2) {
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

// ── Double-precision support for the stall polish ────────────────────────────────────────────
//
// Everything below exists for exactly one caller: the stall polish in gjk() (the finish_stalled
// lambda there carries the full argument). It runs ONLY after the float walk has already stalled
// uncertified — never on the convergence path — so it is written for correctness-by-construction
// rather than speed: a brute-force closest-point-on-hull instead of a second copy of the Voronoi
// case analysis, because a rarely-exercised duplicate of 120 lines of region tests is exactly
// where the next knife-edge bug would live.

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

[[nodiscard]] constexpr Vec3d to_double(core::Vec3 v) noexcept {
    return {v.x, v.y, v.z};
}

[[nodiscard]] constexpr core::Vec3 to_float(Vec3d v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

[[nodiscard]] constexpr Vec3d operator+(Vec3d a, Vec3d b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] constexpr Vec3d operator-(Vec3d a, Vec3d b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] constexpr Vec3d operator*(Vec3d v, double s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

[[nodiscard]] constexpr double dot(Vec3d a, Vec3d b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3d cross(Vec3d a, Vec3d b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Closest point to the ORIGIN on the convex hull of 1–4 points, in double, with barycentrics —
// by brute force over every face of every dimension: each vertex, each edge, each triangle, and
// (given four points) the solid tetrahedron's interior.
//
// The correctness argument is the projection theorem rather than a case analysis: the closest
// point of a convex polytope lies in the relative interior of exactly one face, and for THAT face
// the unconstrained affine projection of the origin lands strictly inside — so enumerating
// "affine projection, kept only if its barycentrics are interior" over ALL faces is guaranteed to
// visit the answer, every candidate kept is genuinely a point of the hull, and the minimum over
// the candidates therefore IS the answer. Boundary cases (a projection landing exactly on an
// edge's endpoint) are covered because the subface is enumerated too.
//
// Degenerate faces — a zero-length edge, a sliver triangle, a flat tetrahedron — are simply
// SKIPPED: their closest points live on their own subfaces, which the enumeration already visits.
// The same "falling back is never wrong" argument as kDegenerateRel above, minus the tuning: here
// nothing needs to be tight, so the guards can sit orders of magnitude above double noise.
struct HullClosest {
    Vec3d point{};
    double lambda[3] = {};
    int index[3] = {}; // indices into the caller's point array
    int count = 0;
    double dist2 = 0.0;
    bool contained = false; // four points, origin strictly inside: overlap, no closest feature
};

[[nodiscard]] inline HullClosest closest_on_hull_double(const Vec3d* p, int n) noexcept {
    HullClosest best;
    bool any = false;
    const auto consider = [&](Vec3d q, int c, const int* idx, const double* lam) {
        const double d2 = dot(q, q);
        if (!any || d2 < best.dist2) {
            any = true;
            best.point = q;
            best.count = c;
            best.dist2 = d2;
            for (int i = 0; i < c; ++i) {
                best.index[i] = idx[i];
                best.lambda[i] = lam[i];
            }
        }
    };

    // Vertices: always valid candidates, and the fallback every degenerate skip relies on.
    for (int i = 0; i < n; ++i) {
        const int idx[1] = {i};
        const double lam[1] = {1.0};
        consider(p[i], 1, idx, lam);
    }

    // Edges: keep the projection only if it lands strictly inside (endpoints are the vertex
    // candidates above). A zero-length edge is skipped outright.
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const Vec3d e = p[j] - p[i];
            const double ee = dot(e, e);
            if (!(ee > 0.0)) {
                continue;
            }
            const double t = -dot(p[i], e) / ee;
            if (t > 0.0 && t < 1.0) {
                const int idx[2] = {i, j};
                const double lam[2] = {1.0 - t, t};
                consider(p[i] + e * t, 2, idx, lam);
            }
        }
    }

    // Triangles: project via the 2x2 Gram system (normal equations of the affine fit). The
    // degeneracy guard is RELATIVE — det = |e1|^2 |e2|^2 sin^2(theta), so this floors the opening
    // angle at ~1e-6 rad, four orders above double's rounding of the same product — and skipping
    // is safe because the triangle's edges are already enumerated.
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            for (int k = j + 1; k < n; ++k) {
                const Vec3d e1 = p[j] - p[i];
                const Vec3d e2 = p[k] - p[i];
                const double a11 = dot(e1, e1);
                const double a12 = dot(e1, e2);
                const double a22 = dot(e2, e2);
                const double det = a11 * a22 - a12 * a12;
                if (!(det > 1e-12 * a11 * a22)) {
                    continue;
                }
                const double b1 = -dot(p[i], e1);
                const double b2 = -dot(p[i], e2);
                const double u = (b1 * a22 - b2 * a12) / det;
                const double v = (b2 * a11 - b1 * a12) / det;
                if (u > 0.0 && v > 0.0 && u + v < 1.0) {
                    const int idx[3] = {i, j, k};
                    const double lam[3] = {1.0 - u - v, u, v};
                    consider(p[i] + e1 * u + e2 * v, 3, idx, lam);
                }
            }
        }
    }

    // The solid tetrahedron: if the origin's barycentrics are strictly interior, the hull
    // encloses it. Claiming containment is the one candidate that must NOT fire on a degenerate
    // face (it would flip a separation into an overlap), so the volume guard errs toward "not
    // contained" — a truly enclosing but flat tetrahedron puts the origin within noise of a face,
    // which the caller's touch exit already reports as overlap.
    if (n == 4) {
        const Vec3d e1 = p[1] - p[0];
        const Vec3d e2 = p[2] - p[0];
        const Vec3d e3 = p[3] - p[0];
        const double det = dot(e1, cross(e2, e3));
        const double scale2 = dot(e1, e1) * dot(e2, e2) * dot(e3, e3);
        if (det * det > 1e-18 * scale2) {
            const Vec3d rhs = p[0] * -1.0;
            const double l1 = dot(rhs, cross(e2, e3)) / det;
            const double l2 = dot(e1, cross(rhs, e3)) / det;
            const double l3 = dot(e1, cross(e2, rhs)) / det;
            if (l1 > 0.0 && l2 > 0.0 && l3 > 0.0 && l1 + l2 + l3 < 1.0) {
                best.contained = true;
            }
        }
    }
    return best;
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

    // The vector from the origin to the closest point of the Minkowski difference — i.e. the
    // SEPARATING AXIS, pointing from B toward A, with `distance` as its length. Mathematically it
    // is exactly `point_a - point_b`; numerically it is a far better answer, and a caller that
    // needs the DIRECTION between the shapes should use this rather than differencing the
    // witnesses.
    //
    // Why they differ: this is accumulated from the simplex's `w` values, which are already
    // differences (support_A - support_B), so the cancellation happens once, early, between two
    // related points. `point_a` and `point_b` are each accumulated separately from raw support
    // points and then subtracted, so a large shape's far-flung support vertices are cancelled
    // twice, independently. On a 100 m wall that is the difference between a correct normal and a
    // diagonal one — and, through conservative advancement's step size, between stopping at a wall
    // and ending up a metre inside it (m12.1). Zero when the shapes overlap: there is no separating
    // axis then.
    core::Vec3 closest{};

    // A guaranteed LOWER BOUND on the true distance, and the field a swept query must step by.
    //
    // `distance` is an UPPER bound: GJK terminates on a simplex, and the closest point of a subset
    // is never nearer than the closest point of the whole set, so an early termination reports a
    // gap slightly WIDER than the truth. That is harmless for a contact test and dangerous for a
    // sweep, where "advance by the gap" then advances by slightly more than the gap — enough to
    // finish INSIDE a large target, or PAST a thin one, where the far side reads as a clean miss.
    //
    // This one comes from the support plane, which is exactly what the convergence test already
    // evaluates: for the search direction v = closest and its support point w, every point p of the
    // Minkowski difference satisfies dot(p, v̂) >= dot(w, v̂), so the origin cannot be nearer than
    // dot(w, v̂). Valid at every iteration, so the largest seen is kept. Never negative, and never
    // greater than `distance`.
    float lower_bound = 0.0f;

    // The unit direction of the support plane that PRODUCED `lower_bound` (same orientation as
    // `closest`: from B toward A). Zero exactly when `lower_bound` is zero.
    //
    // A swept query needs the pair (bound, direction) JOINTLY, not the bound alone: the plane's
    // statement is "no point of the Minkowski difference is nearer than `lower_bound` along THIS
    // direction", so dividing the bound by the sweep's closing rate against this same direction
    // turns a radial bound into a travel bound (the van den Bergen ray-clip step — see the shape
    // cast in src/scene_query.hpp). Dividing by any OTHER direction's closing rate proves nothing,
    // and `lower_bound` is a running max over planes from different iterations — without this
    // field the pairing is lost and the projected step would be built on a category error.
    core::Vec3 plane_dir{};

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

    // The running support-plane bound and the direction of the plane that set it (see
    // GjkResult::lower_bound / plane_dir). Declared before the finish lambdas because they
    // capture them.
    float lower_bound = 0.0f;
    core::Vec3 plane_dir{};

    const auto finish_separated = [&] {
        res.overlapping = false;
        res.point_a = core::Vec3{};
        res.point_b = core::Vec3{};
        for (int i = 0; i < count; ++i) {
            res.point_a += verts[i].a * lambda[i];
            res.point_b += verts[i].b * lambda[i];
        }
        res.distance = core::length(closest);
        res.closest = closest; // the well-conditioned direction — see GjkResult::closest
        res.lower_bound = std::min(lower_bound, res.distance); // a bound, never an over-claim
        res.plane_dir = plane_dir; // the plane the bound came from; clamping only weakens it
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

    // The largest squared support magnitude seen — the scale every certificate below is judged
    // against, because a support-plane bound is a dot product of |w|-sized vectors and its float
    // noise is kSupportEps * |w| (the same scaling the convergence test uses).
    float max_w2 = core::dot(verts[0].w, verts[0].w);

    // The STALL exits' finisher, and the discrimination this whole loop's honesty rests on.
    //
    // A stall exit (duplicate support, no monotone progress, iteration cap) knows only that the
    // walk stopped — and `distance` at that point is an UPPER bound from an arbitrary simplex,
    // which says NOTHING about which side of contact the shapes are on. Measured on the m12.1
    // probe family (sphere r=1 vs a 5x5x0.1 slab): stalls reported "separated, 4.6e-4" at a true
    // penetration of 2.5 MILLIMETRES, and a cast that believed it stopped 3 mm inside the wall
    // (ROADMAP 2026-08-21, the deferred shallow-penetration item).
    //
    // The discriminating fact is GEOMETRY, not tuning: when the origin is inside the Minkowski
    // difference, NO support plane can exclude it — every bound dot(w, v̂) is <= 0 — so a positive
    // plane bound beyond the evaluation's own float noise is a PROOF of separation, and its
    // absence at a stall means "not provably outside". `lower_bound` already holds the best plane
    // seen; the verdict is earned in three steps:
    //
    //   1. The POLISH: restart the distance walk from the stalled simplex with every piece of
    //      SIMPLEX arithmetic in double, and keep the best support-plane bound seen — the
    //      in-loop one or any the polish produces.
    //   2. That bound clears the noise floor: separated, certified, reported FROM THE POLISHED
    //      SIMPLEX. The polish runs even when the in-loop bound alone would certify, because a
    //      certified bound does not launder the stalled simplex's OTHER outputs: its `closest`
    //      (the normal every caller derives), its distance (the upper bound the cast leashes),
    //      and its witnesses are still wreckage. Measured: a retracted normal probe consuming a
    //      certified-but-unpolished stall exit reported a contact normal TRANSVERSE to a 20 m
    //      wall's face (n.x = 4e-4 where -1 was the answer) — the bound was true and everything
    //      else was garbage.
    //   3. Nothing certifies: report OVERLAPPING. Either the shapes truly penetrate (the measured
    //      failure this fixes), or they sit within the noise floor of touching — and a narrowphase
    //      that treats "indistinguishable from contact" as contact errs the way every consumer
    //      here wants: the cast bisects to the boundary by observation, and the contact pipeline
    //      seeds EPA exactly as it does for kTouchEps2 touches.
    //
    // WHY THE POLISH MUST BE A DESCENT, AND WHY IT MUST BE IN DOUBLE — i.e. why nothing cheaper
    // works. Two designs were measured and buried on the way here:
    //
    //   * "Certify along the terminal simplex's own perpendicular." The stalled simplex in the
    //     measured family is a SEGMENT — a chord of the CURVED Minkowski difference (sphere ⊕ box
    //     is curved everywhere the sphere contributes), with endpoints near two far corners of the
    //     slab face, e.g. v0 = (-1.08e-3, -5.011, -5.010), v1 = (-9.5e-4, 5.002, 5.001) at a true
    //     gap of 8.7e-4. A secant of a curved surface is NOT tangent to it: that chord's exact
    //     perpendicular through the origin is 0.13 rad off the true separating axis, and across a
    //     5 m face that tilt costs 0.65 m of plane bound — the certificate evaluates to -0.93 for
    //     a gap of 8.7e-4. No plane through that chord certifies, at ANY precision; the certifying
    //     direction is almost perpendicular to the simplex on hand, so it has to be FOUND, by
    //     iterating, not synthesized from the wreck.
    //   * "Descend in float." The walk stalled precisely because float cannot descend here:
    //     `closest` is a barycentric blend of ±5 m coordinates cancelling to a millimetre, so its
    //     transverse error is ~1 ULP of the SUPPORT coordinates (~5e-7) while the transverse
    //     SIGNAL that should steer the next support is smaller — the search direction's sign is
    //     noise, and the loop flip-flops between opposite corners of the face. In double the same
    //     blend carries ~1e-15 of error, six orders below the signal, and the walk descends like
    //     the textbook algorithm. Measured on the slab family (384 stall-heavy poses): the whole
    //     query — float walk plus polish — spends 11 support evaluations on average, 16 at worst,
    //     and every pose certifies with a bound within 2.2% of the true gap.
    //
    // Precision discipline the polish depends on, easy to break silently:
    //
    //   * Re-difference w = a - b IN DOUBLE from the stored float witnesses. The float `w` was
    //     rounded once already (fl(a-b) carries ~ULP(|b|) of transverse error — the very noise
    //     being escaped); float a and b are EXACT doubles, so the re-difference is exact.
    //   * Round the search direction to FLOAT before evaluating the support, and take the bound
    //     along that same float vector (dot in double). The pair (lower_bound, plane_dir) handed
    //     to the caller is then a statement about the exact plane the caller receives — sound for
    //     ANY direction, because a support plane is a valid bound whatever direction it was queried
    //     along; precision only buys tightness. The only irreducible fuzz left is the support
    //     oracle's own float rounding, which is exactly what the noise floor prices.
    //
    // The verdict stays certificate-shaped: a positive plane bound above the noise floor is a
    // full proof of separation on its own (convergence not required — see the geometry above), and
    // without one the polish's own tetrahedron containment or touch exit reports overlap. The
    // expensive path runs ONLY at stalls; the convergence path never pays for it.
    //
    // kTouchEps2 stays as the fast path above; its absoluteness is harmless now that it is no
    // longer the only touch detector. (The ROADMAP suspicion named the epsilon; the measured
    // culprit was stall exits claiming separation with no certificate — the same absolute-vs-
    // scale disease as #131, in a different organ.)
    const auto finish_stalled = [&] {
        // The double-precision polish, seeded with the stalled simplex. Unconditional — see
        // point 2 above for why a certified in-loop bound is not an excuse to skip it.
        SupportVertex pverts[4];
        Vec3d pw[4];
        int pcount = count;
        for (int i = 0; i < pcount; ++i) {
            pverts[i] = verts[i];
            pw[i] = to_double(verts[i].a) - to_double(verts[i].b); // exact — see above
        }

        double best_bound = 0.0;
        core::Vec3 best_dir{};
        HullClosest sol;
        for (int it = 0; it < kMaxIterations; ++it) {
            sol = closest_on_hull_double(pw, pcount);
            if (sol.contained) {
                // The polished simplex encloses the origin: a genuine overlap, proven the same
                // way the float loop proves it. Adopt the enclosing tetrahedron so EPA seeds from
                // the best available simplex.
                count = pcount;
                for (int i = 0; i < count; ++i) {
                    verts[i] = pverts[i];
                }
                finish_overlapping();
                return;
            }

            // Compact to the supporting feature (copy-out first, same aliasing hazard as
            // solve_simplex). The appended vertex below always lands AFTER the feature, so on
            // every exit pverts[0..sol.count) matches sol's barycentrics.
            SupportVertex keptv[3];
            Vec3d keptw[3];
            for (int i = 0; i < sol.count; ++i) {
                keptv[i] = pverts[sol.index[i]];
                keptw[i] = pw[sol.index[i]];
            }
            for (int i = 0; i < sol.count; ++i) {
                pverts[i] = keptv[i];
                pw[i] = keptw[i];
            }
            pcount = sol.count;

            if (sol.dist2 <= static_cast<double>(kTouchEps2)) {
                count = pcount;
                for (int i = 0; i < count; ++i) {
                    verts[i] = pverts[i];
                }
                finish_overlapping();
                return;
            }

            // The touch exit above floors |closest| at sqrt(kTouchEps2) = 1e-5, so this division
            // can never see a denormal length — the guard a bare `> 0` comparison would not give
            // (1/denormal is inf, which would fabricate an infinite certificate).
            const double len = std::sqrt(sol.dist2);
            const core::Vec3 n = to_float(sol.point * (1.0 / len));
            const SupportVertex sw = minkowski_support(-n);
            max_w2 = std::max(max_w2, core::dot(sw.w, sw.w));
            const Vec3d swd = to_double(sw.a) - to_double(sw.b);

            const double bound = dot(swd, to_double(n));
            if (bound > best_bound) {
                best_bound = bound;
                best_dir = n;
            }

            // Same convergence test as the float loop, evaluated where it can actually resolve:
            // the arithmetic noise term vanishes in double, leaving only the support oracle's
            // float fuzz — which kSupportEps already prices.
            const double fuzz = kSupportEps * std::sqrt(sol.dist2 * dot(swd, swd));
            if (sol.dist2 - dot(sol.point, swd) <= kRelEps * sol.dist2 + fuzz) {
                break;
            }
            bool duplicate = false;
            for (int i = 0; i < pcount; ++i) {
                const Vec3d diff = swd - pw[i];
                if (dot(diff, diff) <= static_cast<double>(kDuplicateEps2)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                break;
            }
            pverts[pcount] = sw;
            pw[pcount] = swd;
            ++pcount;
        }

        // Merge the polish's best plane with the in-loop one — each is independently valid, and
        // the PAIRING travels with whichever bound wins. The noise floor is recomputed here
        // because the polish's own support evaluations may have raised max_w2.
        if (best_bound > static_cast<double>(lower_bound)) {
            lower_bound = static_cast<float>(best_bound);
            plane_dir = best_dir;
        }
        const float noise_floor = kSupportEps * std::sqrt(max_w2);
        if (lower_bound > noise_floor) {
            // Certified. Report the POLISHED feature — its distance is the converged upper bound
            // and its barycentrics reconstruct well-conditioned witnesses — not the stalled wreck.
            count = sol.count;
            for (int i = 0; i < count; ++i) {
                verts[i] = pverts[i];
                lambda[i] = static_cast<float>(sol.lambda[i]);
            }
            closest = to_float(sol.point);
            finish_separated();
            return;
        }
        finish_overlapping();
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
        max_w2 = std::max(max_w2, core::dot(w.w, w.w));

        // The support-plane bound: dot(w, v̂) with v̂ = closest/|closest|. Every point of the
        // Minkowski difference lies on the far side of that plane, so the origin is at least this
        // far away. Kept as the largest seen — each iteration's bound is independently valid.
        {
            const float len = std::sqrt(dist2);
            if (len > 0.0f) {
                const float bound = core::dot(w.w, closest) / len;
                if (bound > lower_bound) {
                    lower_bound = bound;
                    plane_dir = closest * (1.0f / len);
                }
            }
        }

        // Convergence bound: the support plane through w perpendicular to `closest` bounds M, so
        // if w is no closer to the origin than `closest` (up to a tolerance), no point of M is —
        // the origin is outside and `closest` is (within tolerance) the answer. When the origin is
        // INSIDE M this test can never fire: every support along -closest passes the origin
        // (dot(closest, w) <= 0), keeping the left side >= dist2.
        //
        // THE SECOND TERM IS NOT A LOOSENING, IT IS THE FLOOR THIS TEST CAN ACTUALLY RESOLVE.
        // Read the left side as |closest| * (distance - lower_bound): a RELATIVE bound on how much
        // the answer might still improve. But its float error is ABSOLUTE, and set by the size of
        // the shapes rather than the size of the gap — `dot(closest, w.w)` multiplies a small
        // vector by a support point that may be a hundred metres out. So the relative bound alone
        // becomes unreachable once the gap falls below roughly (float eps / kRelEps) * |w|, and
        // GJK cannot stop: it keeps taking supports along a direction whose y/z sign is pure noise,
        // flipping between two opposite corners of the same face, until it has accreted NEAR-
        // DUPLICATE vertices into a degenerate triangle. `closest` then describes that triangle's
        // interior rather than the face — a correct closest point for a simplex that is nonsense.
        //
        // Measured, sphere vs box, before this term existed: at a 1.9e-5 m gap from a 1 m box, GJK
        // returned distance 0.47 for a true gap of 1.9e-5, with a normal perpendicular to the true
        // one. The failing gaps scale with the box, exactly as the model predicts — below ~1.4e-3 m
        // for a 1 m box, below ~0.14 m for a 100 m box. That is the defect that reached m12.1's
        // shape cast as a diagonal contact normal, and it was NEVER an arm64 bug; arm64 merely
        // rounded its way across the threshold in a case x86 survived.
        //
        // Stopping here is not giving up: at this point the simplex is still the healthy one, and
        // `closest` is right. It is CONTINUING that destroys the answer.
        const float support_noise = kSupportEps * std::sqrt(dist2 * core::dot(w.w, w.w));
        if (dist2 - core::dot(closest, w.w) <= kRelEps * dist2 + support_noise) {
            finish_separated();
            return res;
        }

        // Cycling guard: re-encountering a vertex means fp noise is driving the loop, not
        // geometry. A stall, so the verdict must be EARNED — see finish_stalled: "separated" here
        // is only an upper bound from an arbitrary simplex, and believing it uncertified is how a
        // 2.5 mm penetration read as a 4.6e-4 gap.
        bool duplicate = false;
        for (int i = 0; i < count; ++i) {
            const core::Vec3 diff = w.w - verts[i].w;
            if (core::dot(diff, diff) <= kDuplicateEps2) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            finish_stalled();
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
        // progress, stop instead of spinning — but the verdict must be earned (finish_stalled):
        // this is the exit that carried every misread in the shallow-penetration family.
        if (new_dist2 >= dist2) {
            finish_stalled();
            return res;
        }
        dist2 = new_dist2;
    }

    // Iteration cap (never hit in practice at our scales; a safety net, not a code path we rely
    // on). Report the current state honestly: near-zero distance as overlap, else a stall —
    // "separated" from here is subject to exactly the certificate finish_stalled demands.
    if (dist2 <= kTouchEps2) {
        finish_overlapping();
    } else {
        finish_stalled();
    }
    return res;
}

} // namespace rime::physics
