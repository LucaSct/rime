// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/shape.hpp"

// Axis-aligned bounding boxes: the currency of the broadphase (M7.2) and the ray/sweep queries
// (M7.7). An AABB is the cheapest useful bound — six floats and a handful of comparisons to test
// overlap — so the broadphase culls the O(n²) pair space down to a handful of candidates that the
// exact narrowphase (M7.3) then confirms.
namespace rime::physics {

struct Aabb {
    core::Vec3 min{0.0f, 0.0f, 0.0f};
    core::Vec3 max{0.0f, 0.0f, 0.0f};
};

// Two AABBs overlap iff they overlap on every axis (the separating-axis test, specialized to the
// three world axes). Touching counts as overlap (≤/≥) so coincident faces are not missed.
[[nodiscard]] inline bool overlaps(const Aabb& a, const Aabb& b) noexcept {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// Smallest AABB containing both — the bound an internal BVH node gets from its two children.
[[nodiscard]] inline Aabb merge(const Aabb& a, const Aabb& b) noexcept {
    return Aabb{
        {std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
        {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

// True if `outer` fully contains `inner` (used by the tree's "fat AABB still valid?" fast path).
[[nodiscard]] inline bool contains(const Aabb& outer, const Aabb& inner) noexcept {
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           outer.max.x >= inner.max.x && outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}

// Surface area — the cost metric the tree's insertion heuristic (SAH) minimizes: a node's expected
// query cost is proportional to its surface area, so growing area least keeps the tree cheap.
[[nodiscard]] inline float surface_area(const Aabb& a) noexcept {
    const float dx = a.max.x - a.min.x;
    const float dy = a.max.y - a.min.y;
    const float dz = a.max.z - a.min.z;
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

// The AABB grown by `margin` on every side (the "fat" AABB the broadphase stores so a body that
// moves a little stays inside its bound and needs no re-insert).
[[nodiscard]] inline Aabb expanded(const Aabb& a, float margin) noexcept {
    const core::Vec3 m{margin, margin, margin};
    return Aabb{a.min - m, a.max + m};
}

// The tight world AABB of a posed shape. Sphere ignores orientation; an oriented box's world extent
// along each axis is the abs-sum of its rotated local half-edges (the standard OBB→AABB); a capsule
// is the union of its two end-spheres.
[[nodiscard]] inline Aabb
compute_aabb(const ShapeDesc& s, core::Vec3 pos, const core::Quat& q) noexcept {
    switch (s.type) {
        case ShapeType::Sphere: {
            const core::Vec3 r{s.radius, s.radius, s.radius};
            return Aabb{pos - r, pos + r};
        }
        case ShapeType::Box: {
            const core::Vec3 ex = core::rotate(q, core::Vec3{s.half_extents.x, 0.0f, 0.0f});
            const core::Vec3 ey = core::rotate(q, core::Vec3{0.0f, s.half_extents.y, 0.0f});
            const core::Vec3 ez = core::rotate(q, core::Vec3{0.0f, 0.0f, s.half_extents.z});
            const core::Vec3 h{std::fabs(ex.x) + std::fabs(ey.x) + std::fabs(ez.x),
                               std::fabs(ex.y) + std::fabs(ey.y) + std::fabs(ez.y),
                               std::fabs(ex.z) + std::fabs(ey.z) + std::fabs(ez.z)};
            return Aabb{pos - h, pos + h};
        }
        case ShapeType::Capsule: {
            const core::Vec3 axis = core::rotate(q, core::Vec3{0.0f, s.half_height, 0.0f});
            const core::Vec3 p0 = pos - axis;
            const core::Vec3 p1 = pos + axis;
            const core::Vec3 lo{std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::min(p0.z, p1.z)};
            const core::Vec3 hi{std::max(p0.x, p1.x), std::max(p0.y, p1.y), std::max(p0.z, p1.z)};
            const core::Vec3 r{s.radius, s.radius, s.radius};
            return Aabb{lo - r, hi + r};
        }
    }
    return Aabb{pos, pos};
}

} // namespace rime::physics
