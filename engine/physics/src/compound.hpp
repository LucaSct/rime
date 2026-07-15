// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "hull.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/aabb.hpp"
#include "rime/physics/shape.hpp"

// Compound shapes (M7.12, ADR-0028) — the runtime form of a registered compound and everything
// derived at registration: validated children, and the COMPOSED mass properties (total volume,
// combined centre of mass, parallel-axis inertia, principal axes; derivation in
// docs/math/compound-mass-properties.md).
//
// The storage model is ADR-0027's, applied again: the child list is WORLD-OWNED. A caller
// registers children once through the PhysicsWorld seam and gets a small CompoundId back;
// ShapeDesc carries only that id, so it stays a flat POD. This header is the store's internal
// currency — PRIVATE under src/, invisible above the seam. Registration is cold-path (once per
// fracture *pattern*, not per body), so it can afford validation and an eigendecomposition;
// everything the per-tick hot path needs (re-centred child poses, per-unit-mass principal
// moments) is precomputed into flat arrays.
//
// Determinism: every routine below is a pure function of the authored child list (and the hull
// store entries it references) — fixed scan orders, no unordered containers — so identical
// registration calls yield bit-identical compounds (ADR-0026).
namespace rime::physics {

namespace compound_detail {

// v1 child cap: generous headroom for fracture-cell counts (a destructible wall is tens of
// cells), while child indices travel in 16 bits everywhere (Manifold::child_a/b, cache sub-keys,
// events), so the cap can grow ×256 without any format change. Registration rejects beyond it.
inline constexpr std::uint32_t kMaxCompoundChildren = 256;

// A child orientation quaternion must have usable length before we normalize it (an all-zero
// quaternion normalizes to NaN and would silently poison every posed vertex downstream).
inline constexpr float kMinQuatLength2 = 1e-8f;

} // namespace compound_detail

// One registered compound, in its RUNTIME frame: child poses re-centred so the combined centre of
// mass is the local origin (ADR-0028 — the engine-wide "body position IS the centre of mass"
// invariant holds for compounds by construction, exactly as hulls did it). Children are stored in
// authored order; a child's index here IS the stable id that manifolds, cache keys, and contact
// events carry (registration-fixed, hence deterministic). Parallel arrays, SoA-style, because the
// narrowphase touches poses and shapes in tight per-pair loops.
struct CompoundShape {
    std::vector<ShapeDesc> child_shape;   // primitive or ConvexHull ref — never Compound (v1)
    std::vector<core::Vec3> child_pos;    // in the compound frame, relative to the COM
    std::vector<core::Quat> child_orient; // normalized at registration

    // Derived physical properties (registration-time; docs/math/compound-mass-properties.md).
    float volume = 0.0f;                            // total child volume (uniform density v1)
    core::Vec3 centroid_authored{0.0f, 0.0f, 0.0f}; // COM in the AUTHORED frame (the shift)
    core::Vec3 inertia_per_mass{1.0f, 1.0f, 1.0f};  // principal moments of a 1 kg body (I ∝ m)
    core::Quat principal = core::quat_identity();   // principal frame → compound local frame

    [[nodiscard]] std::size_t child_count() const noexcept { return child_shape.size(); }
};

// Resolve a child's hull reference against the world's hull store, passed as a span so this
// header stays free of world internals. Mirrors world.cpp's hull_of: null for primitives and for
// an unresolvable id (the store is append-only, so generation is always 0).
[[nodiscard]] inline const ConvexHull*
compound_child_hull(const ShapeDesc& s, std::span<const ConvexHull> hulls) noexcept {
    if (s.type != ShapeType::ConvexHull) {
        return nullptr;
    }
    if (s.hull.index >= hulls.size() || s.hull.generation != 0) {
        return nullptr;
    }
    return &hulls[s.hull.index];
}

// Pose composition: a child rides rigidly on its body, so the child's world pose is the body pose
// applied to the child's local pose — rotate the stored offset by the body orientation, add the
// body position; orientations multiply (body ∘ child, right-to-left like all our rotation
// composition). This is the one place the composition is written down; broadphase bounds,
// narrowphase dispatch, CCD, and the queries all call it so a child can never be posed two
// different ways.
[[nodiscard]] inline core::Vec3 compound_child_world_pos(const CompoundShape& c,
                                                         std::size_t i,
                                                         core::Vec3 body_pos,
                                                         const core::Quat& body_q) noexcept {
    return body_pos + core::rotate(body_q, c.child_pos[i]);
}

[[nodiscard]] inline core::Quat compound_child_world_orient(const CompoundShape& c,
                                                            std::size_t i,
                                                            const core::Quat& body_q) noexcept {
    return body_q * c.child_orient[i];
}

// Tight world AABB of a posed compound: the union of the posed children's tight bounds. O(total
// child geometry) — fine at fracture-cell counts, and only recomputed when a body actually moved
// (the step's refit discipline). This is the ONE broadphase bound a compound body owns (model A,
// ADR-0028): the proxy↔body mapping stays 1:1 and the per-child precision is recovered inside the
// narrowphase by the child AABB cull.
[[nodiscard]] inline Aabb compound_world_aabb(const CompoundShape& c,
                                              std::span<const ConvexHull> hulls,
                                              core::Vec3 pos,
                                              const core::Quat& q) noexcept {
    Aabb bounds{};
    for (std::size_t i = 0; i < c.child_count(); ++i) {
        const core::Vec3 cp = compound_child_world_pos(c, i, pos, q);
        const core::Quat cq = compound_child_world_orient(c, i, q);
        const ConvexHull* h = compound_child_hull(c.child_shape[i], hulls);
        const Aabb child =
            h != nullptr ? hull_world_aabb(*h, cp, cq) : compute_aabb(c.child_shape[i], cp, cq);
        bounds = i == 0 ? child : merge(bounds, child);
    }
    return bounds;
}

namespace compound_detail {

// Solid volume of one child, uniform density. The capsule contributes its v1 CYLINDER volume —
// deliberately the same approximation as its v1 cylinder inertia (shape.hpp), so the mass model
// is one consistent story: when the hemispherical caps are folded into the inertia later, the
// volume gains them in the same change. A hull's volume was integrated at its registration.
[[nodiscard]] inline float child_volume(const ShapeDesc& s, const ConvexHull* hull) noexcept {
    switch (s.type) {
        case ShapeType::Sphere:
            return (4.0f / 3.0f) * std::numbers::pi_v<float> * s.radius * s.radius * s.radius;
        case ShapeType::Box:
            return 8.0f * s.half_extents.x * s.half_extents.y * s.half_extents.z;
        case ShapeType::Capsule:
            return std::numbers::pi_v<float> * s.radius * s.radius * (2.0f * s.half_height);
        case ShapeType::ConvexHull:
            return hull != nullptr ? hull->volume : 0.0f;
        case ShapeType::Compound:
            return 0.0f; // nesting is rejected before volumes are ever asked for
    }
    return 0.0f;
}

// Rotate a DIAGONAL per-unit-mass inertia into the compound frame: I' = R·diag(j)·Rᵀ. Writing R's
// columns c_m = R·e_m makes the identity transparent — R·diag(j)·Rᵀ = Σ_m j_m·(c_m·c_mᵀ), i.e.
// each principal moment contributes a rank-1 outer product along its (rotated) axis. Six unique
// entries of the symmetric result, accumulated straight into hull_detail's SymMat3 so the Jacobi
// solver from M7.11 diagonalizes the sum unchanged.
[[nodiscard]] inline hull_detail::SymMat3 rotate_diagonal_inertia(const core::Quat& r,
                                                                  core::Vec3 j) noexcept {
    const core::Vec3 c0 = core::rotate(r, core::Vec3{1.0f, 0.0f, 0.0f});
    const core::Vec3 c1 = core::rotate(r, core::Vec3{0.0f, 1.0f, 0.0f});
    const core::Vec3 c2 = core::rotate(r, core::Vec3{0.0f, 0.0f, 1.0f});
    hull_detail::SymMat3 m;
    m.xx = j.x * c0.x * c0.x + j.y * c1.x * c1.x + j.z * c2.x * c2.x;
    m.yy = j.x * c0.y * c0.y + j.y * c1.y * c1.y + j.z * c2.y * c2.y;
    m.zz = j.x * c0.z * c0.z + j.y * c1.z * c1.z + j.z * c2.z * c2.z;
    m.xy = j.x * c0.x * c0.y + j.y * c1.x * c1.y + j.z * c2.x * c2.y;
    m.xz = j.x * c0.x * c0.z + j.y * c1.x * c1.z + j.z * c2.x * c2.z;
    m.yz = j.x * c0.y * c0.z + j.y * c1.y * c1.z + j.z * c2.y * c2.z;
    return m;
}

} // namespace compound_detail

// Validate an authored child list and build the runtime CompoundShape (ADR-0028). Returns false —
// and leaves `out` untouched — on ANY violation; registration never repairs input (the
// register_hull posture: an authoring bug fails loudly at register time, not as a wobble
// mid-simulation). The checks, in order:
//   1. child count in [1, kMaxCompoundChildren] (ONE child is legal — a deliberate COM-offset
//      shape, the use ADR-0027 promised compounds would be the honest home for);
//   2. per child: not itself a Compound (nesting rejected in v1 — flatten-at-register is its
//      deferred home, ADR-0028), a resolvable hull id for ConvexHull children, an orientation of
//      usable length (normalized on store), and a non-degenerate volume;
//   3. positive total volume and strictly positive composed principal moments.
// On success the stored child poses are RE-CENTRED on the combined COM and all derived data is
// filled in.
[[nodiscard]] inline bool build_compound(std::span<const CompoundChildDesc> children,
                                         std::span<const ConvexHull> hulls,
                                         CompoundShape& out) {
    using namespace compound_detail;

    // ---- 1 + 2. Structure and per-child validity (one pass; volumes remembered for the mass
    // composition below).
    if (children.empty() || children.size() > kMaxCompoundChildren) {
        return false;
    }
    std::vector<float> volumes(children.size());
    std::vector<core::Quat> orients(children.size());
    float total_volume = 0.0f;
    for (std::size_t i = 0; i < children.size(); ++i) {
        const CompoundChildDesc& child = children[i];
        if (child.shape.type == ShapeType::Compound) {
            return false; // no nesting in v1 (ADR-0028 defers flatten-at-register)
        }
        const ConvexHull* h = compound_child_hull(child.shape, hulls);
        if (child.shape.type == ShapeType::ConvexHull && h == nullptr) {
            return false; // a hull child must reference a hull THIS world knows
        }
        if (!(core::dot(child.orientation, child.orientation) > kMinQuatLength2)) {
            return false; // a zero-ish quaternion has no direction to normalize toward
        }
        orients[i] = core::normalize(child.orientation);
        const float v = child_volume(child.shape, h);
        if (!(v > hull_detail::kMinVolume)) {
            return false; // a volumeless child would contribute NaN-adjacent mass fractions
        }
        volumes[i] = v;
        total_volume += v;
    }
    if (!(total_volume > hull_detail::kMinVolume)) {
        return false;
    }

    // ---- 3. Combined centre of mass. Uniform density (the v1 mass model — one material across a
    // destructible's cells) makes each child's mass fraction its volume fraction. A child's own
    // COM sits at its stored local origin — primitives by symmetry, hulls because THEIR
    // registration already re-centred them (ADR-0027) — so a child's COM in the compound frame is
    // simply its authored position. That is the quiet payoff of the hull re-centring decision: no
    // per-child COM offset ever needs tracking here.
    const float inv_volume = 1.0f / total_volume;
    core::Vec3 com{0.0f, 0.0f, 0.0f};
    for (std::size_t i = 0; i < children.size(); ++i) {
        com += children[i].position * (volumes[i] * inv_volume);
    }

    // ---- 4. Composed inertia about the COM, per unit TOTAL mass. Each child of mass fraction
    // f_i contributes f_i · (R_i·diag(J_i)·R_iᵀ + |d_i|²·E − d_i·d_iᵀ): its own per-unit-mass
    // inertia rotated into the compound frame, plus the PARALLEL-AXIS shift for carrying it at
    // offset d_i from the combined COM (docs/math/compound-mass-properties.md §2). R_i composes
    // the child pose with, for hull children, the hull's principal rotation — the stored hull
    // diagonal lives in the hull's principal frame, so principal → hull-local → compound.
    hull_detail::SymMat3 inertia;
    for (std::size_t i = 0; i < children.size(); ++i) {
        const CompoundChildDesc& child = children[i];
        const float f = volumes[i] * inv_volume;

        core::Vec3 j;
        core::Quat r = orients[i];
        if (const ConvexHull* h = compound_child_hull(child.shape, hulls); h != nullptr) {
            j = h->inertia_per_mass;
            r = r * h->principal;
        } else {
            j = compute_mass_properties(child.shape, 1.0f).inertia_diagonal;
        }
        const hull_detail::SymMat3 rotated = rotate_diagonal_inertia(r, j);

        const core::Vec3 d = children[i].position - com;
        const float d2 = core::dot(d, d);
        inertia.xx += f * (rotated.xx + d2 - d.x * d.x);
        inertia.yy += f * (rotated.yy + d2 - d.y * d.y);
        inertia.zz += f * (rotated.zz + d2 - d.z * d.z);
        inertia.xy += f * (rotated.xy - d.x * d.y);
        inertia.xz += f * (rotated.xz - d.x * d.z);
        inertia.yz += f * (rotated.yz - d.y * d.z);
    }

    // Diagonalize to principal moments + axes — the M7.11 machinery, reused (fixed-sweep Jacobi,
    // det +1 by construction, identity for an already-diagonal tensor).
    core::Vec3 moments;
    float axes[3][3];
    hull_detail::jacobi_diagonalize(inertia, moments, axes);
    if (!(moments.x > 0.0f) || !(moments.y > 0.0f) || !(moments.z > 0.0f)) {
        return false; // solid children at finite offsets compose strictly positive moments
    }

    // ---- Fill the runtime compound: child poses re-centred on the COM (so the compound's local
    // origin IS its centre of mass), everything else as derived.
    out.child_shape.resize(children.size());
    out.child_pos.resize(children.size());
    out.child_orient = std::move(orients);
    for (std::size_t i = 0; i < children.size(); ++i) {
        out.child_shape[i] = children[i].shape;
        out.child_pos[i] = children[i].position - com;
    }
    out.volume = total_volume;
    out.centroid_authored = com;
    out.inertia_per_mass = moments;
    out.principal = hull_detail::quat_from_columns(axes);
    return true;
}

} // namespace rime::physics
