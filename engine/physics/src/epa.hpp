// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>
#include <vector>

#include "gjk.hpp"
#include "rime/core/math/vec.hpp"

// EPA (Expanding Polytope Algorithm) — penetration depth and direction for two OVERLAPPING convex
// shapes (derivation: docs/math/gjk-epa.md). GJK answers "do they overlap"; EPA answers "by how
// much, and along which direction is the overlap smallest" — exactly the (normal, depth) a contact
// manifold needs.
//
// The idea: when the shapes overlap, the origin lies INSIDE the Minkowski difference M = A - B,
// and the penetration vector is the shortest translation taking the origin to M's boundary. EPA
// finds it by growing a convex polytope inside M, seeded from GJK's terminal simplex: repeatedly
// take the polytope face nearest the origin, ask the support function for M's real extent along
// that face's normal, and if M extends further, split the polytope open (remove every face visible
// from the new point — the same "horizon" operation as incremental convex hull) and stitch in a
// fan of new faces. When the nearest face cannot be pushed any further, that face IS (a facet of)
// M's boundary: its normal is the contact normal, its distance the penetration depth.
//
// Robustness posture (documented, not hidden): float-only, metre-scale tolerances; face normals
// are oriented by the seed polytope's centroid (always interior, so orientation never flips as the
// polytope grows); degenerate GJK simplices are inflated to a tetrahedron by probing fixed axis
// directions before expansion starts; on any numeric dead end (broken horizon, vertex/face budget)
// EPA stops and returns the best face found so far rather than looping. Good enough for the
// primitive-shape regime of M7.3 — re-audited when convex hulls (M7.9) widen the input space.
//
// Private header (under src/), invisible above the PhysicsWorld seam.
namespace rime::physics {

namespace epa_detail {

// Metre-scale calibration, same caveat as GJK's: kGrowthEps stops expanding when a support point
// pushes a face out by less than a tenth of a millimetre (contact depth accuracy), kPlaneEps is
// the "is this face visible from the new vertex" plane-side slop, kFeatureEps2 detects duplicate
// support points (the difference is flat in that direction — converged).
inline constexpr float kGrowthEps = 1e-4f;
inline constexpr float kPlaneEps = 1e-5f;
inline constexpr float kFeatureEps2 = 1e-12f;
inline constexpr int kMaxIterations = 64;
inline constexpr int kMaxVertices = 64;

struct Face {
    int i0, i1, i2;
    core::Vec3 normal; // unit, oriented away from the polytope interior
    float dist;        // distance of the face plane from the origin along `normal`
    bool alive;
};

struct Edge {
    int a, b;
};

} // namespace epa_detail

struct EpaResult {
    bool valid = false;
    core::Vec3 normal{0.0f, 1.0f, 0.0f}; // unit; push B along it (A along -it) to separate
    float depth = 0.0f;                  // penetration depth along `normal`, >= 0
    core::Vec3 point_a{};                // deepest-overlap witness on A's surface
    core::Vec3 point_b{};                // witness on B's surface
};

// Run EPA seeded with GJK's terminal simplex (any 1–4 vertices whose hull passes through/around
// the origin). Support callables must be the SAME ones the GJK call used.
template <class SupA, class SupB>
[[nodiscard]] EpaResult
epa(const SupA& support_a, const SupB& support_b, const SupportVertex* seed, int seed_count) {
    using namespace epa_detail;

    const auto minkowski_support = [&](core::Vec3 d) -> SupportVertex {
        SupportVertex sv;
        sv.a = support_a(d);
        sv.b = support_b(-d);
        sv.w = sv.a - sv.b;
        return sv;
    };

    EpaResult res;
    if (seed_count < 1) {
        return res;
    }

    // --- Inflate the seed to a non-degenerate tetrahedron -------------------------------------
    // GJK can terminate "overlapping" with fewer than four vertices (the origin landed ON a
    // vertex/edge/triangle of the walk), and even a 4-vertex terminal simplex can be numerically
    // flat. EPA's expansion needs a genuine 3-D starting polytope, so build one by AFFINE RANK:
    // take seed vertices while each strictly increases the rank (a distinct point, then a point
    // off the line, then a point off the plane), and when the seed runs dry, probe fixed axis
    // directions — deterministically ordered, so the same scene always builds the same polytope.
    // Every pair we feed EPA has a volumetric Minkowski difference (each pair involves a 3-D
    // shape), so this only fails if the difference is numerically flat — then we bail and the
    // caller copes (drops the contact this tick).
    std::vector<SupportVertex> verts;
    verts.reserve(kMaxVertices);

    // Rank test for a candidate against the vertices accepted so far. Thresholds are squared
    // metre-scale lengths/areas (kFeatureEps2-relative where a scale exists).
    const auto extends_rank = [&](const SupportVertex& w) -> bool {
        switch (verts.size()) {
            case 0:
                return true;
            case 1: {
                const core::Vec3 d = w.w - verts[0].w;
                return core::dot(d, d) > kFeatureEps2;
            }
            case 2: {
                const core::Vec3 e = verts[1].w - verts[0].w;
                const core::Vec3 off = core::cross(w.w - verts[0].w, e);
                return core::dot(off, off) > kFeatureEps2 * core::dot(e, e);
            }
            default: {
                const core::Vec3 n = core::cross(verts[1].w - verts[0].w, verts[2].w - verts[0].w);
                const float n2 = core::dot(n, n);
                const float off = core::dot(w.w - verts[0].w, n);
                return off * off > kFeatureEps2 * n2;
            }
        }
    };

    for (int i = 0; i < seed_count && verts.size() < 4; ++i) {
        if (extends_rank(seed[i])) {
            verts.push_back(seed[i]);
        }
    }
    static constexpr core::Vec3 kProbes[6] = {{1.0f, 0.0f, 0.0f},
                                              {-1.0f, 0.0f, 0.0f},
                                              {0.0f, 1.0f, 0.0f},
                                              {0.0f, -1.0f, 0.0f},
                                              {0.0f, 0.0f, 1.0f},
                                              {0.0f, 0.0f, -1.0f}};
    while (verts.size() < 4) {
        bool grew = false;
        if (verts.size() <= 1) {
            // Any direction with nonzero extent yields a second vertex; a 3-D set has one among
            // the cardinals.
            for (const core::Vec3& d : kProbes) {
                const SupportVertex w = minkowski_support(d);
                if (extends_rank(w)) {
                    verts.push_back(w);
                    grew = true;
                    break;
                }
            }
        } else if (verts.size() == 2) {
            // Probe PERPENDICULAR to the edge (cross with each cardinal): probing raw cardinals
            // can fail — e.g. a box whose main diagonal is axis-aligned answers all six cardinal
            // supports with its two diagonal corners, which sit exactly on our edge. The
            // edge-perpendicular fan sweeps a great circle of directions, which cannot keep
            // returning collinear supports for a volumetric set.
            const core::Vec3 e = verts[1].w - verts[0].w;
            for (const core::Vec3& axis : kProbes) {
                const core::Vec3 d = core::cross(e, axis);
                if (core::dot(d, d) <= kFeatureEps2 * core::dot(e, e)) {
                    continue; // cardinal ~parallel to the edge — nothing perpendicular here
                }
                const SupportVertex w = minkowski_support(d);
                if (extends_rank(w)) {
                    verts.push_back(w);
                    grew = true;
                    break;
                }
            }
        } else {
            // A plane exists: its own normal is the only productive probe direction.
            const core::Vec3 n = core::cross(verts[1].w - verts[0].w, verts[2].w - verts[0].w);
            for (const core::Vec3 d : {n, -n}) {
                const SupportVertex w = minkowski_support(d);
                if (extends_rank(w)) {
                    verts.push_back(w);
                    grew = true;
                    break;
                }
            }
        }
        if (!grew) {
            return res; // numerically flat difference: no polytope to expand
        }
    }

    // The seed centroid is interior to the tetrahedron and STAYS interior as the polytope only
    // ever grows outward — a stable reference for orienting every face normal away from the
    // inside, immune to the origin sitting exactly on a face (the touching case).
    const core::Vec3 interior = (verts[0].w + verts[1].w + verts[2].w + verts[3].w) * 0.25f;

    std::vector<Face> faces;
    faces.reserve(128);
    const auto push_face = [&](int i0, int i1, int i2) -> bool {
        const core::Vec3 a = verts[static_cast<std::size_t>(i0)].w;
        core::Vec3 n = core::cross(verts[static_cast<std::size_t>(i1)].w - a,
                                   verts[static_cast<std::size_t>(i2)].w - a);
        const float len = core::length(n);
        if (len <= 1e-12f) {
            return false; // sliver face — skip; the polytope stays a (slightly gappy) best effort
        }
        n = n * (1.0f / len);
        int j1 = i1;
        int j2 = i2;
        if (core::dot(n, a - interior) < 0.0f) { // orient outward
            n = -n;
            j1 = i2;
            j2 = i1;
        }
        faces.push_back(Face{i0, j1, j2, n, core::dot(n, a), true});
        return true;
    };
    if (!push_face(0, 1, 2) || !push_face(0, 1, 3) || !push_face(0, 2, 3) || !push_face(1, 2, 3)) {
        return res;
    }

    // --- Expansion loop ------------------------------------------------------------------------
    std::vector<Edge> horizon;
    int best_face = -1;
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        // Nearest alive face to the origin. Deterministic tie-break: the first (lowest index)
        // wins, and face order is itself deterministic — same scene, same polytope, same answer.
        best_face = -1;
        float best_dist = 0.0f;
        for (std::size_t f = 0; f < faces.size(); ++f) {
            if (!faces[f].alive) {
                continue;
            }
            if (best_face < 0 || faces[f].dist < best_dist) {
                best_face = static_cast<int>(f);
                best_dist = faces[f].dist;
            }
        }
        if (best_face < 0) {
            return res; // no faces left (numerics ate the polytope) — give up honestly
        }

        const core::Vec3 n = faces[static_cast<std::size_t>(best_face)].normal;
        const SupportVertex w = minkowski_support(n);
        const float growth = core::dot(n, w.w) - best_dist;

        // Converged: M does not extend meaningfully past this face, so the face lies ON M's
        // boundary — its normal/distance are the penetration direction/depth.
        if (growth <= kGrowthEps || verts.size() >= kMaxVertices) {
            break;
        }
        bool duplicate = false;
        for (const SupportVertex& v : verts) {
            const core::Vec3 diff = w.w - v.w;
            if (core::dot(diff, diff) <= kFeatureEps2) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            break; // support returned an existing vertex: no expansion possible along n
        }

        // Remove every face visible from w and collect the HORIZON — the boundary between removed
        // and kept faces. Directed edges of removed faces cancel pairwise where two removed faces
        // meet, so the surviving edges form the closed loop around the hole, wound consistently
        // (each edge kept the winding of its removed face). The nearest face is removed
        // unconditionally: growth > 0 proved w is beyond it, whatever the epsilon says.
        horizon.clear();
        const auto add_horizon_edge = [&](int a, int b) {
            for (std::size_t e = 0; e < horizon.size(); ++e) {
                if (horizon[e].a == b && horizon[e].b == a) {
                    horizon[e] = horizon.back();
                    horizon.pop_back();
                    return;
                }
            }
            horizon.push_back(Edge{a, b});
        };
        for (std::size_t f = 0; f < faces.size(); ++f) {
            Face& face = faces[f];
            if (!face.alive) {
                continue;
            }
            const bool visible =
                static_cast<int>(f) == best_face ||
                core::dot(face.normal, w.w - verts[static_cast<std::size_t>(face.i0)].w) >
                    kPlaneEps;
            if (!visible) {
                continue;
            }
            face.alive = false;
            add_horizon_edge(face.i0, face.i1);
            add_horizon_edge(face.i1, face.i2);
            add_horizon_edge(face.i2, face.i0);
        }
        if (horizon.empty()) {
            break; // degenerate visibility (shouldn't happen): keep the best answer so far
        }

        const int wi = static_cast<int>(verts.size());
        verts.push_back(w);
        for (const Edge& e : horizon) {
            (void)push_face(e.a, e.b, wi);
        }
    }

    if (best_face < 0) {
        return res;
    }

    // Read the answer off the best face. The witness points come from the same barycentric trick
    // as GJK: express the origin's projection onto the face in barycentric coordinates of the
    // face's Minkowski vertices, then apply those weights to the stored per-shape support points.
    const Face& face = faces[static_cast<std::size_t>(best_face)];
    SupportVertex tri[3] = {verts[static_cast<std::size_t>(face.i0)],
                            verts[static_cast<std::size_t>(face.i1)],
                            verts[static_cast<std::size_t>(face.i2)]};
    const gjk_detail::FeatureClosest fc = gjk_detail::closest_on_triangle(tri, 0, 1, 2);
    res.point_a = core::Vec3{};
    res.point_b = core::Vec3{};
    for (int i = 0; i < fc.count; ++i) {
        res.point_a += tri[fc.index[i]].a * fc.lambda[i];
        res.point_b += tri[fc.index[i]].b * fc.lambda[i];
    }
    res.normal = face.normal;
    res.depth = face.dist > 0.0f ? face.dist : 0.0f;
    res.valid = true;
    return res;
}

} // namespace rime::physics
