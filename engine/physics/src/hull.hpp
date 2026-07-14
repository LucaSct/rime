// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/aabb.hpp"

// Convex hulls (M7.11, ADR-0027) — the runtime form of a registered hull and everything derived
// from it at registration time: validated topology, face planes, and the polyhedral mass
// properties (volume, centre of mass, principal moments + axes; derivation in
// docs/math/polyhedral-mass-properties.md).
//
// The storage model (ADR-0027): hull geometry is WORLD-OWNED. A caller registers vertex/face spans
// once through the PhysicsWorld seam and gets a small HullId back; ShapeDesc carries only that id,
// so it stays a flat POD. This header is the store's internal currency — like the rest of src/ it
// is PRIVATE, invisible above the seam. Registration is cold-path (per fracture *pattern*, not per
// body), so it can afford full validation and an eigendecomposition; everything the per-tick hot
// path needs is precomputed here into flat arrays.
//
// Determinism: every routine below is a pure function of the authored spans — fixed scan orders,
// first-wins ties, no unordered containers — so identical registration calls yield bit-identical
// hulls (and downstream, identical support points, manifolds, and world hashes; ADR-0026).
namespace rime::physics {

namespace hull_detail {

// A face may carry at most this many vertices: the narrowphase clips an incident face against a
// reference face's side planes in FIXED stack buffers (allocation-free contact path), and each
// convex clip adds at most one vertex net — so incident cap + reference cap bounds the polygon at
// 32, well inside the clip buffers. Fracture cells (Voronoi faces) run 4–10 vertices in practice;
// 16 is generous headroom, and registration rejects beyond it rather than truncating silently.
inline constexpr std::uint32_t kMaxHullFaceVertices = 16;

// Metre-scale tolerances, in the same calibration family as GJK/EPA's (see gjk.hpp): a face is
// planar / a vertex is "behind" a plane within a tenth of a millimetre; a hull under one cubic
// millimetre of volume is degenerate (its inertia would vanish into float noise).
inline constexpr float kPlaneEps = 1e-4f;
inline constexpr float kMinVolume = 1e-9f;
inline constexpr float kDegenerateNormal2 = 1e-12f;

} // namespace hull_detail

// One registered convex hull, in its RUNTIME frame: vertices re-centred so the centre of mass is
// the local origin (ADR-0027 — the engine-wide "body position IS the centre of mass" invariant
// then holds for hulls by construction). Faces are CSR: face f's vertex indices are
// face_indices[face_offsets[f] .. face_offsets[f + 1]), wound outward (counter-clockwise viewed
// from outside), exactly as authored. Face planes are dot(face_normal[f], x) = face_plane_d[f]
// with the normal unit and outward.
struct ConvexHull {
    std::vector<core::Vec3> vertices;
    std::vector<std::uint32_t> face_offsets; // size = face count + 1
    std::vector<std::uint32_t> face_indices;
    std::vector<core::Vec3> face_normals; // unit, outward, local frame
    std::vector<float> face_plane_d;      // plane offset: dot(n, any face vertex)

    // Derived physical properties (registration-time; docs/math/polyhedral-mass-properties.md).
    float volume = 0.0f;
    core::Vec3 centroid_authored{0.0f, 0.0f, 0.0f}; // COM in the AUTHORED frame (the shift applied)
    core::Vec3 inertia_per_mass{1.0f, 1.0f, 1.0f};  // principal moments of a 1 kg body (I ∝ m)
    core::Quat principal = core::quat_identity();   // rotates principal frame → hull local frame

    [[nodiscard]] std::size_t face_count() const noexcept { return face_offsets.size() - 1; }
};

// Farthest hull vertex along a LOCAL direction — the support function GJK/EPA run on
// (src/support.hpp): a hull answers the one question every convex shape answers, by scanning its
// vertices. Strict '>' keeps the FIRST best vertex on ties (a direction perpendicular to a face
// ties across that whole face), so the pick is a pure function of the stored vertex order —
// the same determinism discipline as sign_nonneg() for boxes. O(V) linear scan: correct first;
// hill-climbing over a vertex adjacency is the measured-need optimization ADR-0026 defers.
[[nodiscard]] inline core::Vec3 hull_support_local(const ConvexHull& h, core::Vec3 dir) noexcept {
    std::size_t best = 0;
    float best_d = core::dot(h.vertices[0], dir);
    for (std::size_t i = 1; i < h.vertices.size(); ++i) {
        const float d = core::dot(h.vertices[i], dir);
        if (d > best_d) {
            best_d = d;
            best = i;
        }
    }
    return h.vertices[best];
}

// Tight world AABB of a posed hull: pose each vertex and take component-wise min/max. Exact (a
// convex hull's extreme points are vertices), and O(V) on the same modest vertex counts as the
// support scan.
[[nodiscard]] inline Aabb
hull_world_aabb(const ConvexHull& h, core::Vec3 pos, const core::Quat& q) noexcept {
    core::Vec3 lo = pos + core::rotate(q, h.vertices[0]);
    core::Vec3 hi = lo;
    for (std::size_t i = 1; i < h.vertices.size(); ++i) {
        const core::Vec3 w = pos + core::rotate(q, h.vertices[i]);
        lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
        hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
    }
    return Aabb{lo, hi};
}

namespace hull_detail {

// --------------------------------------------------------------- polyhedral mass properties ----
// A symmetric 3×3 accumulated as its six unique entries. Used for both the second-moment
// (covariance) matrix C = ∫ x xᵀ dV and the inertia tensor derived from it.
struct SymMat3 {
    float xx = 0.0f, yy = 0.0f, zz = 0.0f;
    float xy = 0.0f, xz = 0.0f, yz = 0.0f;
};

// Volume, centre of mass, and covariance of a closed polyhedron by SIGNED TETRAHEDRON
// DECOMPOSITION (the divergence-theorem method): fan-triangulate every face and, for each surface
// triangle (a, b, c), form the tetrahedron it spans with the ORIGIN. Its SIGNED volume is
// det[a b c] / 6 — positive when the outward face winding faces away from the origin, negative
// when it faces toward it — and summing signed contributions makes everything outside the solid
// cancel exactly, wherever the origin sits. The same trick gives the first moment (centroid) and
// second moment: over one origin-tetrahedron the closed forms are
//     ∫ x dV     = V · (a + b + c) / 4                                   (centroid of a tet)
//     ∫ x xᵀ dV  = V / 20 · (aaᵀ + bbᵀ + ccᵀ + ssᵀ),   s = a + b + c
// (both derived step by step in docs/math/polyhedral-mass-properties.md), and both inherit the
// signed-cancellation property because they scale linearly with the signed V. Density is uniform
// and folded out: we integrate with ρ = 1, so "mass" here is the volume, and the caller
// renormalizes to the body's real mass afterwards.
struct VolumeIntegrals {
    float volume = 0.0f;
    core::Vec3 first_moment{0.0f, 0.0f, 0.0f}; // ∫ x dV; centroid = this / volume
    SymMat3 covariance;                        // ∫ x xᵀ dV about the authored origin
};

[[nodiscard]] inline VolumeIntegrals
integrate_polyhedron(std::span<const core::Vec3> verts,
                     std::span<const std::uint32_t> face_offsets,
                     std::span<const std::uint32_t> face_indices) noexcept {
    VolumeIntegrals out;
    for (std::size_t f = 0; f + 1 < face_offsets.size(); ++f) {
        const std::uint32_t begin = face_offsets[f];
        const std::uint32_t end = face_offsets[f + 1];
        const core::Vec3 a = verts[face_indices[begin]];
        for (std::uint32_t k = begin + 1; k + 1 < end; ++k) {
            const core::Vec3 b = verts[face_indices[k]];
            const core::Vec3 c = verts[face_indices[k + 1]];
            // Signed tetra volume: det[a b c] / 6 = a · (b × c) / 6.
            const float v6 = core::dot(a, core::cross(b, c));
            const float vol = v6 / 6.0f;
            out.volume += vol;
            const core::Vec3 s = a + b + c;
            out.first_moment += s * (vol * 0.25f); // V · (a+b+c+0)/4
            const float k20 = vol / 20.0f;
            // aaᵀ + bbᵀ + ccᵀ + ssᵀ, scaled by V/20 — six unique entries of the symmetric matrix.
            out.covariance.xx += k20 * (a.x * a.x + b.x * b.x + c.x * c.x + s.x * s.x);
            out.covariance.yy += k20 * (a.y * a.y + b.y * b.y + c.y * c.y + s.y * s.y);
            out.covariance.zz += k20 * (a.z * a.z + b.z * b.z + c.z * c.z + s.z * s.z);
            out.covariance.xy += k20 * (a.x * a.y + b.x * b.y + c.x * c.y + s.x * s.y);
            out.covariance.xz += k20 * (a.x * a.z + b.x * b.z + c.x * c.z + s.x * s.z);
            out.covariance.yz += k20 * (a.y * a.z + b.y * b.z + c.y * c.z + s.y * s.z);
        }
    }
    return out;
}

// -------------------------------------------------------------------- Jacobi eigensolver -------
// Diagonalize a symmetric 3×3 by CYCLIC JACOBI ROTATIONS: repeatedly apply a Givens rotation in
// the (p, q) plane chosen to zero the off-diagonal entry A[p][q] exactly (A ← Jᵀ A J), sweeping
// the three off-diagonal entries in a FIXED order. Each rotation can re-grow previously zeroed
// entries, but the off-diagonal Frobenius norm shrinks monotonically and the method converges
// quadratically — a fixed 12 sweeps takes a 3×3 far past float precision, and a fixed count (not a
// convergence early-out) matches the house determinism style (solver iteration counts, ADR-0026).
// The accumulated V is a product of rotations, so it is a PROPER rotation (det +1) by
// construction — its columns are the eigenvectors and it converts to a quaternion with no
// handedness fix-up. For an already-diagonal input every pivot is zero, every rotation is skipped,
// and V stays exactly identity: an axis-aligned box-shaped hull reports the identity principal
// rotation, bit for bit.
inline void jacobi_diagonalize(const SymMat3& m, core::Vec3& eigenvalues, float v[3][3]) noexcept {
    float a[3][3] = {{m.xx, m.xy, m.xz}, {m.xy, m.yy, m.yz}, {m.xz, m.yz, m.zz}};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            v[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
    constexpr int kSweeps = 12;
    constexpr int kPairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
    for (int sweep = 0; sweep < kSweeps; ++sweep) {
        for (const auto& pair : kPairs) {
            const int p = pair[0];
            const int q = pair[1];
            const float apq = a[p][q];
            if (apq == 0.0f) {
                continue; // already exactly zero — rotating would be a no-op
            }
            // The rotation angle that zeroes a[p][q]: tan(2θ) = 2·apq / (aqq − app). The stable
            // half-angle form (Golub & Van Loan): t = sign(θ̂)/(|θ̂| + √(θ̂² + 1)) picks the
            // SMALLER of the two zeroing angles, which is what keeps the iteration contractive.
            const float theta = (a[q][q] - a[p][p]) / (2.0f * apq);
            const float t = (theta >= 0.0f ? 1.0f : -1.0f) /
                            (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
            const float c = 1.0f / std::sqrt(t * t + 1.0f);
            const float s = t * c;

            const float app = a[p][p];
            const float aqq = a[q][q];
            a[p][p] = c * c * app - 2.0f * s * c * apq + s * s * aqq;
            a[q][q] = s * s * app + 2.0f * s * c * apq + c * c * aqq;
            a[p][q] = 0.0f; // zeroed by construction — store the exact zero
            a[q][p] = 0.0f;
            for (int r = 0; r < 3; ++r) {
                if (r == p || r == q) {
                    continue;
                }
                const float arp = a[r][p];
                const float arq = a[r][q];
                a[r][p] = c * arp - s * arq;
                a[p][r] = a[r][p];
                a[r][q] = s * arp + c * arq;
                a[q][r] = a[r][q];
            }
            for (int r = 0; r < 3; ++r) {
                const float vrp = v[r][p];
                const float vrq = v[r][q];
                v[r][p] = c * vrp - s * vrq;
                v[r][q] = s * vrp + c * vrq;
            }
        }
    }
    eigenvalues = {a[0][0], a[1][1], a[2][2]};
}

// Rotation matrix (columns = principal axes) → quaternion, by Shepperd's method: of the four
// quaternion components, compute the one the matrix trace/diagonal says is LARGEST first (so the
// divisions below are well-conditioned — never a divide by a near-zero component), then read the
// other three off the matrix's symmetric/antisymmetric parts. Branches compare computed values, so
// the result is a pure function of the input (deterministic), and the input is det +1 by
// construction (Jacobi accumulates rotations), so no reflection case exists.
[[nodiscard]] inline core::Quat quat_from_columns(const float v[3][3]) noexcept {
    const float trace = v[0][0] + v[1][1] + v[2][2];
    core::Quat q;
    if (trace > 0.0f) {
        const float r = std::sqrt(1.0f + trace);
        const float inv = 0.5f / r;
        q.w = 0.5f * r;
        q.x = (v[2][1] - v[1][2]) * inv;
        q.y = (v[0][2] - v[2][0]) * inv;
        q.z = (v[1][0] - v[0][1]) * inv;
    } else if (v[0][0] >= v[1][1] && v[0][0] >= v[2][2]) {
        const float r = std::sqrt(1.0f + v[0][0] - v[1][1] - v[2][2]);
        const float inv = 0.5f / r;
        q.x = 0.5f * r;
        q.w = (v[2][1] - v[1][2]) * inv;
        q.y = (v[0][1] + v[1][0]) * inv;
        q.z = (v[0][2] + v[2][0]) * inv;
    } else if (v[1][1] >= v[2][2]) {
        const float r = std::sqrt(1.0f - v[0][0] + v[1][1] - v[2][2]);
        const float inv = 0.5f / r;
        q.y = 0.5f * r;
        q.w = (v[0][2] - v[2][0]) * inv;
        q.x = (v[0][1] + v[1][0]) * inv;
        q.z = (v[1][2] + v[2][1]) * inv;
    } else {
        const float r = std::sqrt(1.0f - v[0][0] - v[1][1] + v[2][2]);
        const float inv = 0.5f / r;
        q.z = 0.5f * r;
        q.w = (v[1][0] - v[0][1]) * inv;
        q.x = (v[0][2] + v[2][0]) * inv;
        q.y = (v[1][2] + v[2][1]) * inv;
    }
    return core::normalize(q);
}

// Newell's method for a polygon normal: sum the cross-product-like edge terms
//     n += (cur − next) ⊗ ...   (component form below)
// over the closed loop. For an exactly planar polygon it equals (twice the area times) the plane
// normal from any convex corner; for a slightly non-planar one it gives the least-squares plane —
// robust where a single cross(b−a, c−a) can pick a degenerate corner. Orientation follows the
// winding (counter-clockwise viewed from outside ⇒ outward).
[[nodiscard]] inline core::Vec3 newell_normal(std::span<const core::Vec3> verts,
                                              std::span<const std::uint32_t> idx) noexcept {
    core::Vec3 n{0.0f, 0.0f, 0.0f};
    for (std::size_t i = 0; i < idx.size(); ++i) {
        const core::Vec3 cur = verts[idx[i]];
        const core::Vec3 nxt = verts[idx[(i + 1) % idx.size()]];
        n.x += (cur.y - nxt.y) * (cur.z + nxt.z);
        n.y += (cur.z - nxt.z) * (cur.x + nxt.x);
        n.z += (cur.x - nxt.x) * (cur.y + nxt.y);
    }
    return n;
}

} // namespace hull_detail

// Validate authored hull geometry and build the runtime ConvexHull (ADR-0027). Returns false —
// and leaves `out` untouched — on ANY violation; registration never repairs input (a fracture
// cook's bug should fail loudly at register time, not wobble a simulation later). The checks, in
// order:
//   1. structure: ≥ 4 vertices, ≥ 4 faces, 3..kMaxHullFaceVertices vertices per face, all indices
//      in range;
//   2. closed 2-manifold with consistent winding: every DIRECTED edge appears exactly once and its
//      reverse exists (each undirected edge shared by exactly two faces, traversed in opposite
//      directions);
//   3. planar faces with a well-defined normal;
//   4. convex and outward-wound: EVERY hull vertex lies on or behind EVERY face plane (within
//      kPlaneEps). This one check catches both dents and inward winding — an inward-wound face's
//      "outward" Newell normal flips, and the far side of the hull lands in front of it;
//   5. positive, non-degenerate volume.
// On success the stored vertices are RE-CENTRED on the centre of mass and all derived data
// (planes, mass properties, principal axes) is filled in.
[[nodiscard]] inline bool build_convex_hull(std::span<const core::Vec3> vertices,
                                            std::span<const std::uint32_t> face_counts,
                                            std::span<const std::uint32_t> face_indices,
                                            ConvexHull& out) {
    using namespace hull_detail;

    // ---- 1. Structure.
    if (vertices.size() < 4 || face_counts.size() < 4) {
        return false;
    }
    std::size_t total = 0;
    for (const std::uint32_t c : face_counts) {
        if (c < 3 || c > kMaxHullFaceVertices) {
            return false;
        }
        total += c;
    }
    if (face_indices.size() != total) {
        return false;
    }
    for (const std::uint32_t i : face_indices) {
        if (i >= vertices.size()) {
            return false;
        }
    }

    // CSR offsets (face f = indices [offsets[f], offsets[f+1])).
    std::vector<std::uint32_t> offsets(face_counts.size() + 1, 0);
    for (std::size_t f = 0; f < face_counts.size(); ++f) {
        offsets[f + 1] = offsets[f] + face_counts[f];
    }

    // ---- 2. Closed 2-manifold, consistent winding. Collect every directed edge as a packed
    // 64-bit key and sort: a duplicate directed edge means two faces traverse it the same way
    // (inconsistent winding or worse); a missing reverse means an open boundary. Sorted-vector
    // + binary search keeps this deterministic and allocation-light (cold path anyway).
    std::vector<std::uint64_t> edges;
    edges.reserve(total);
    for (std::size_t f = 0; f < face_counts.size(); ++f) {
        const std::uint32_t begin = offsets[f];
        const std::uint32_t count = face_counts[f];
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t a = face_indices[begin + k];
            const std::uint32_t b = face_indices[begin + (k + 1) % count];
            if (a == b) {
                return false; // a zero-length edge (repeated index) is degenerate
            }
            edges.push_back((static_cast<std::uint64_t>(a) << 32) | b);
        }
    }
    std::sort(edges.begin(), edges.end());
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        if (edges[i] == edges[i + 1]) {
            return false; // same directed edge twice
        }
    }
    for (const std::uint64_t e : edges) {
        const std::uint64_t rev = (e << 32) | (e >> 32);
        if (!std::binary_search(edges.begin(), edges.end(), rev)) {
            return false; // open boundary: an edge with no opposite-direction twin
        }
    }

    // ---- 3 + 4. Face planes; convexity and outwardness. Newell normal per face, then every
    // vertex of the hull must lie on or behind every face plane. Also check each face's own
    // vertices actually lie ON its plane (planarity) — clipping and raycast treat faces as flat.
    std::vector<core::Vec3> normals(face_counts.size());
    std::vector<float> plane_d(face_counts.size());
    for (std::size_t f = 0; f < face_counts.size(); ++f) {
        const std::span<const std::uint32_t> idx = face_indices.subspan(offsets[f], face_counts[f]);
        const core::Vec3 nn = newell_normal(vertices, idx);
        const float len2 = core::dot(nn, nn);
        if (len2 <= kDegenerateNormal2) {
            return false; // collinear/degenerate face
        }
        const core::Vec3 n = nn * (1.0f / std::sqrt(len2));
        const float d = core::dot(n, vertices[idx[0]]);
        for (const std::uint32_t vi : idx) {
            if (std::fabs(core::dot(n, vertices[vi]) - d) > kPlaneEps) {
                return false; // non-planar face
            }
        }
        normals[f] = n;
        plane_d[f] = d;
    }
    for (std::size_t f = 0; f < face_counts.size(); ++f) {
        for (const core::Vec3& v : vertices) {
            if (core::dot(normals[f], v) - plane_d[f] > kPlaneEps) {
                return false; // a vertex in FRONT of a face: dented, or wound inward
            }
        }
    }

    // ---- 5. Mass properties (docs/math/polyhedral-mass-properties.md): integrate with unit
    // density about the authored origin, recover the centre of mass, shift the covariance to it
    // (∫(x−d)(x−d)ᵀ = C − M·ddᵀ, the covariance form of the parallel-axis theorem), normalize to
    // per-unit-mass (inertia scales linearly with mass for fixed geometry), turn covariance into
    // inertia (I = tr(C)·1 − C), and diagonalize to principal moments + axes.
    const VolumeIntegrals vi = integrate_polyhedron(vertices, offsets, face_indices);
    if (!(vi.volume > kMinVolume)) {
        return false;
    }
    const float inv_vol = 1.0f / vi.volume;
    const core::Vec3 com = vi.first_moment * inv_vol;

    SymMat3 cov = vi.covariance; // → about the COM, per unit mass (unit density ⇒ mass = volume)
    cov.xx = (cov.xx - vi.volume * com.x * com.x) * inv_vol;
    cov.yy = (cov.yy - vi.volume * com.y * com.y) * inv_vol;
    cov.zz = (cov.zz - vi.volume * com.z * com.z) * inv_vol;
    cov.xy = (cov.xy - vi.volume * com.x * com.y) * inv_vol;
    cov.xz = (cov.xz - vi.volume * com.x * com.z) * inv_vol;
    cov.yz = (cov.yz - vi.volume * com.y * com.z) * inv_vol;

    SymMat3 inertia; // I = tr(C)·identity − C  (e.g. I_xx = ∫(y² + z²), I_xy = −∫xy)
    const float tr = cov.xx + cov.yy + cov.zz;
    inertia.xx = tr - cov.xx;
    inertia.yy = tr - cov.yy;
    inertia.zz = tr - cov.zz;
    inertia.xy = -cov.xy;
    inertia.xz = -cov.xz;
    inertia.yz = -cov.yz;

    core::Vec3 moments;
    float axes[3][3];
    jacobi_diagonalize(inertia, moments, axes);
    if (!(moments.x > 0.0f) || !(moments.y > 0.0f) || !(moments.z > 0.0f)) {
        return false; // a solid with volume has strictly positive moments; anything else is noise
    }

    // ---- Fill the runtime hull: vertices re-centred on the COM, planes recomputed in the
    // re-centred frame (normals are translation-invariant; offsets shift by dot(n, com)).
    out.vertices.assign(vertices.begin(), vertices.end());
    for (core::Vec3& v : out.vertices) {
        v -= com;
    }
    out.face_offsets = std::move(offsets);
    out.face_indices.assign(face_indices.begin(), face_indices.end());
    out.face_normals = std::move(normals);
    out.face_plane_d = std::move(plane_d);
    for (std::size_t f = 0; f < out.face_plane_d.size(); ++f) {
        out.face_plane_d[f] -= core::dot(out.face_normals[f], com);
    }
    out.volume = vi.volume;
    out.centroid_authored = com;
    out.inertia_per_mass = moments;
    out.principal = quat_from_columns(axes);
    return true;
}

} // namespace rime::physics
