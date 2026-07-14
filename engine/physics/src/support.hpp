// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>

#include "hull.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/shape.hpp"

// Support functions — the one primitive GJK and EPA (src/gjk.hpp, src/epa.hpp) need from a shape.
//
// The support point of a convex set S in direction d is the point of S farthest along d:
//     support_S(d) = argmax_{p in S} dot(p, d).
// It is the entire interface between "geometry" and "collision algorithm": GJK and EPA never see
// vertices, faces, or radii — only this one query. That is why the same two algorithms run convex
// hulls (M7.11) unchanged: a hull answers the same question by scanning its vertices
// (hull_support_local in src/hull.hpp).
//
// Two properties the algorithms lean on:
//  - support of a sum/offset: support_{A+t}(d) = support_A(d) + t (posing = rotate d into local
//    space, take the local support, rotate back and translate);
//  - support of a Minkowski DIFFERENCE: support_{A-B}(d) = support_A(d) - support_B(-d). The
//    difference set A - B = { a - b } contains the origin exactly when A and B overlap, which is
//    the reformulation GJK/EPA are built on (derivation: docs/math/gjk-epa.md).
//
// This header lives under src/ (PRIVATE): like aabb_tree.hpp it is an implementation detail,
// invisible above the PhysicsWorld seam.
namespace rime::physics {

// sign(x) with sign(0) = +1. The tie matters: a support direction exactly parallel to a box face
// touches the whole face, and ANY vertex of that face is a valid support point — but determinism
// requires the SAME one every time, on every platform. Branching on >= 0 (never on the sign bit
// of a computed value like -0.0f vs +0.0f is avoided by comparing with >=) makes the pick a pure
// function of the input value.
[[nodiscard]] inline float sign_nonneg(float x) noexcept {
    return x >= 0.0f ? 1.0f : -1.0f;
}

// Farthest point of a LOCAL-space shape along a LOCAL-space direction. `dir` need not be unit —
// the argmax is scale-invariant — which saves normalizations in the GJK loop. `hull` must be the
// resolved geometry when (and only when) s.type == ConvexHull: a ShapeDesc carries just the hull
// id (ADR-0027), so the caller — always inside the seam, where the store lives — resolves it.
[[nodiscard]] inline core::Vec3
support_local(const ShapeDesc& s, core::Vec3 dir, const ConvexHull* hull = nullptr) noexcept {
    switch (s.type) {
        case ShapeType::Sphere: {
            // A sphere's support is r * dir_hat. Guard the zero direction (normalize() would
            // return zero; any surface point is valid, so pick +X deterministically).
            const float len = core::length(dir);
            if (len <= 1e-12f) {
                return core::Vec3{s.radius, 0.0f, 0.0f};
            }
            return dir * (s.radius / len);
        }
        case ShapeType::Box: {
            // A box's support is the corner whose sign pattern matches dir's: independent argmax
            // per axis because the box is an axis product of intervals.
            return core::Vec3{sign_nonneg(dir.x) * s.half_extents.x,
                              sign_nonneg(dir.y) * s.half_extents.y,
                              sign_nonneg(dir.z) * s.half_extents.z};
        }
        case ShapeType::Capsule: {
            // A capsule = core segment (local Y, +/- half_height) Minkowski-summed with a sphere
            // of its radius, so its support = segment support + sphere support (sums add).
            const float len = core::length(dir);
            const core::Vec3 ball =
                len > 1e-12f ? dir * (s.radius / len) : core::Vec3{s.radius, 0.0f, 0.0f};
            return core::Vec3{0.0f, sign_nonneg(dir.y) * s.half_height, 0.0f} + ball;
        }
        case ShapeType::ConvexHull:
            // Scan the registered vertices (hull.hpp). A null hull here is a caller bug (the
            // world never dispatches an unresolved hull); return the origin rather than crash.
            return hull != nullptr ? hull_support_local(*hull, dir) : core::Vec3{};
    }
    return core::Vec3{};
}

// Farthest point of a POSED shape along a WORLD-space direction: rotate the query into local
// space (the inverse rotation), answer there, pose the answer back. This is the general form;
// the GJK/EPA entry points below wrap concrete shapes so the hot loop carries no switch.
[[nodiscard]] inline core::Vec3 support_world(const ShapeDesc& s,
                                              core::Vec3 pos,
                                              const core::Quat& q,
                                              core::Vec3 dir,
                                              const ConvexHull* hull = nullptr) noexcept {
    const core::Vec3 local_dir = core::rotate(core::conjugate(q), dir);
    return pos + core::rotate(q, support_local(s, local_dir, hull));
}

// A posed convex shape as a support-function object — the form GJK/EPA take (templated on the
// callable, so the compiler inlines the whole support chain into the loop; no virtual dispatch
// on the contact hot path). `hull` is the resolved geometry for a ConvexHull shape and stays
// nullptr for primitives — added LAST so the M7.3-era three-field aggregate initializers keep
// meaning exactly what they meant.
struct ShapeSupport {
    const ShapeDesc* shape;
    core::Vec3 pos;
    core::Quat orient;
    const ConvexHull* hull = nullptr;

    [[nodiscard]] core::Vec3 operator()(core::Vec3 dir) const noexcept {
        return support_world(*shape, pos, orient, dir, hull);
    }
};

// A world-space segment as a support function. Used by the capsule fast paths: a capsule is its
// core segment "inflated" by its radius, so colliding capsule-vs-X reduces to measuring the
// distance from the SEGMENT to X and comparing against the radius — the shrunk-shape trick
// (docs/math/gjk-epa.md, section on fast paths). Segments are also degenerate (flat) shapes GJK
// handles fine but EPA must be careful with; see epa.hpp.
struct SegmentSupport {
    core::Vec3 p0;
    core::Vec3 p1;

    [[nodiscard]] core::Vec3 operator()(core::Vec3 dir) const noexcept {
        return core::dot(dir, p1 - p0) >= 0.0f ? p1 : p0;
    }
};

} // namespace rime::physics
