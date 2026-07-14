// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

#include "compound.hpp"
#include "gjk.hpp"
#include "hull.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/shape.hpp"
#include "support.hpp"

// Exact ray-vs-shape and sphere-vs-shape geometry for the scene queries (M7.7). The broadphase BVH
// (aabb_tree.hpp) narrows a query to a handful of candidate leaves; these routines are the exact
// test each candidate then gets. Analytic for the primitives — a ray against them is a quadratic
// or a slab test — plus, since M7.11, the convex generalizations for hulls (a face-plane slab
// test for rays, a GJK distance for the sphere overlap), so a query costs little per candidate and
// stays GPU-free like the rest of the module. This header lives under src/ (PRIVATE), invisible
// above the PhysicsWorld seam.
//
// Convention: `dir` is UNIT, so the returned `t` is a world-space distance; every routine rotates
// the ray into the shape's local frame (a rotation is an isometry, so `t` is unchanged) where the
// shape is axis-aligned and the algebra is simplest, then rotates the surface normal back out.
namespace rime::physics {

// Ray vs sphere: solve |o + t·d − c|² = r². Nearest non-negative root in [0, tmax]; a ray starting
// inside returns t = 0. Normal is the outward radial direction at the hit.
[[nodiscard]] inline bool ray_vs_sphere(core::Vec3 center,
                                        float r,
                                        core::Vec3 o,
                                        core::Vec3 d,
                                        float tmax,
                                        float& t_out,
                                        core::Vec3& n_out) noexcept {
    const core::Vec3 m = o - center;
    const float b = core::dot(m, d);
    const float c = core::dot(m, m) - r * r;
    // Origin outside the sphere (c > 0) and the ray pointing away from it (b > 0): a clean miss.
    if (c > 0.0f && b > 0.0f) {
        return false;
    }
    const float disc = b * b - c; // d is unit ⇒ the t²-coefficient is 1
    if (disc < 0.0f) {
        return false;
    }
    float t = -b - std::sqrt(disc);
    if (t < 0.0f) {
        t = 0.0f; // origin inside the sphere
    }
    if (t > tmax) {
        return false;
    }
    t_out = t;
    n_out = core::normalize((o + d * t) - center);
    return true;
}

// Ray vs oriented box (half-extents `half`, pose `pos`/`q`). Rotate the ray into the box's local
// frame and run the slab test, tracking which axis-slab is entered last — that face gives the
// normal. Returns the entry distance; a ray whose origin is already inside the box reports no
// exterior hit (documented: pick a start point outside).
[[nodiscard]] inline bool ray_vs_box(core::Vec3 half,
                                     core::Vec3 pos,
                                     const core::Quat& q,
                                     core::Vec3 o,
                                     core::Vec3 d,
                                     float tmax,
                                     float& t_out,
                                     core::Vec3& n_out) noexcept {
    const core::Quat qc = core::conjugate(q);
    const core::Vec3 loV = core::rotate(qc, o - pos);
    const core::Vec3 ldV = core::rotate(qc, d);
    const float lo[3] = {loV.x, loV.y, loV.z};
    const float ld[3] = {ldV.x, ldV.y, ldV.z};
    const float h[3] = {half.x, half.y, half.z};

    float tmin = 0.0f;
    float tout = tmax;
    int axis = -1;
    float sign = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ld[i]) < 1e-8f) {
            if (lo[i] < -h[i] || lo[i] > h[i]) {
                return false; // parallel to this slab and outside it
            }
            continue;
        }
        const float inv = 1.0f / ld[i];
        float t1 = (-h[i] - lo[i]) * inv;
        float t2 = (h[i] - lo[i]) * inv;
        float s = -1.0f; // entering the −face
        if (t1 > t2) {
            std::swap(t1, t2);
            s = 1.0f; // …the +face
        }
        if (t1 > tmin) {
            tmin = t1;
            axis = i;
            sign = s;
        }
        tout = std::min(tout, t2);
        if (tmin > tout) {
            return false;
        }
    }
    if (axis < 0) {
        return false; // origin inside (no slab was entered after t=0)
    }
    t_out = tmin;
    float nl[3] = {0.0f, 0.0f, 0.0f};
    nl[axis] = sign;
    n_out = core::rotate(q, core::Vec3{nl[0], nl[1], nl[2]});
    return true;
}

// Ray vs capsule (radius `r`, cylinder half-height `hh` along local Y, pose `pos`/`q`). The capsule
// surface is the cylindrical side over y ∈ [−hh, hh] plus the two hemispherical caps; test the
// infinite cylinder (clamped to the segment) and the two end spheres (accepting only each outer
// hemisphere), and take the nearest. This is exact — the capsule is precisely that union.
[[nodiscard]] inline bool ray_vs_capsule(float r,
                                         float hh,
                                         core::Vec3 pos,
                                         const core::Quat& q,
                                         core::Vec3 o,
                                         core::Vec3 d,
                                         float tmax,
                                         float& t_out,
                                         core::Vec3& n_out) noexcept {
    const core::Quat qc = core::conjugate(q);
    const core::Vec3 lo = core::rotate(qc, o - pos);
    const core::Vec3 ld = core::rotate(qc, d);

    float best = tmax;
    bool hit = false;
    core::Vec3 nloc{0.0f, 0.0f, 0.0f};

    // Infinite cylinder about local Y: drop the Y component and solve the 2-D circle equation. `a`
    // is not 1 here (the xz-projection of a unit ray is not unit), so keep the full quadratic.
    const float a = ld.x * ld.x + ld.z * ld.z;
    if (a > 1e-12f) {
        const float b = lo.x * ld.x + lo.z * ld.z;
        const float c = lo.x * lo.x + lo.z * lo.z - r * r;
        const float disc = b * b - a * c;
        if (disc >= 0.0f) {
            const float t = (-b - std::sqrt(disc)) / a;
            const float y = lo.y + t * ld.y;
            if (t >= 0.0f && t < best && y >= -hh && y <= hh) {
                best = t;
                hit = true;
                nloc = core::Vec3{lo.x + t * ld.x, 0.0f, lo.z + t * ld.z};
            }
        }
    }

    // End caps: spheres at (0, ±hh, 0), accepting a hit only on the hemisphere beyond the segment.
    const auto cap = [&](float cy) {
        const core::Vec3 center{0.0f, cy, 0.0f};
        const core::Vec3 m = lo - center;
        const float b = core::dot(m, ld);
        const float c = core::dot(m, m) - r * r;
        if (c > 0.0f && b > 0.0f) {
            return;
        }
        const float disc = b * b - c;
        if (disc < 0.0f) {
            return;
        }
        float t = -b - std::sqrt(disc);
        if (t < 0.0f) {
            t = 0.0f;
        }
        if (t >= best) {
            return;
        }
        const float y = lo.y + t * ld.y;
        if ((cy < 0.0f && y <= -hh) || (cy > 0.0f && y >= hh)) {
            best = t;
            hit = true;
            nloc = (lo + ld * t) - center;
        }
    };
    cap(-hh);
    cap(hh);

    if (!hit) {
        return false;
    }
    t_out = best;
    n_out = core::rotate(q, core::normalize(nloc));
    return true;
}

// Ray vs convex hull (M7.11) — the slab test generalized from three axis slabs to the hull's
// face planes: a convex polyhedron is the intersection of its faces' half-spaces, so the ray is
// inside exactly on the intersection of the per-plane parameter intervals. Track the LATEST entry
// (that face is where the ray pierces the surface — its normal is the hit normal) and the
// EARLIEST exit; a miss is an empty interval. Same posture as ray_vs_box for an inside origin: no
// entering plane ⇒ no exterior hit reported.
[[nodiscard]] inline bool ray_vs_hull(const ConvexHull& h,
                                      core::Vec3 pos,
                                      const core::Quat& q,
                                      core::Vec3 o,
                                      core::Vec3 d,
                                      float tmax,
                                      float& t_out,
                                      core::Vec3& n_out) noexcept {
    const core::Quat qc = core::conjugate(q);
    const core::Vec3 lo = core::rotate(qc, o - pos);
    const core::Vec3 ld = core::rotate(qc, d);

    float t_enter = 0.0f;
    float t_exit = tmax;
    std::ptrdiff_t enter_face = -1;
    for (std::size_t f = 0; f < h.face_normals.size(); ++f) {
        const core::Vec3 n = h.face_normals[f];
        const float dist = core::dot(n, lo) - h.face_plane_d[f]; // > 0 ⇒ outside this half-space
        const float denom = core::dot(n, ld); // rate the ray gains distance against the plane
        if (std::fabs(denom) < 1e-8f) {
            if (dist > 0.0f) {
                return false; // parallel to the plane and on the outside: can never enter
            }
            continue; // parallel and inside: this plane never constrains the interval
        }
        const float t = -dist / denom;
        if (denom < 0.0f) {
            // Heading INTO the half-space: t is where the ray crosses in. The latest such
            // crossing is the surface hit.
            if (t > t_enter) {
                t_enter = t;
                enter_face = static_cast<std::ptrdiff_t>(f);
            }
        } else if (t < t_exit) {
            t_exit = t; // heading out: the earliest crossing out closes the interval
        }
        if (t_enter > t_exit) {
            return false;
        }
    }
    if (enter_face < 0) {
        return false; // origin inside the hull (no plane was entered after t = 0)
    }
    t_out = t_enter;
    n_out = core::rotate(q, h.face_normals[static_cast<std::size_t>(enter_face)]);
    return true;
}

// Dispatch a ray at any shape. Fills (t, normal) and returns true on the nearest hit in
// [0, tmax]; `dir` must be unit. `hull` is the resolved store entry for a ConvexHull shape
// (nullptr otherwise — the world resolves the id before dispatch, ADR-0027). `compound` + `hulls`
// are the resolved compound and the hull store span for a Compound shape (M7.12): a compound is
// raycast child by child — nearest child hit wins, fixed ascending scan with strict '<' so an
// exact tie keeps the lowest child index (the house determinism discipline). Children are never
// compounds (rejected at registration), so the recursion below is exactly one level deep.
[[nodiscard]] inline bool ray_vs_shape(const ShapeDesc& s,
                                       core::Vec3 pos,
                                       const core::Quat& q,
                                       core::Vec3 o,
                                       core::Vec3 dir,
                                       float tmax,
                                       float& t_out,
                                       core::Vec3& n_out,
                                       const ConvexHull* hull = nullptr,
                                       const CompoundShape* compound = nullptr,
                                       std::span<const ConvexHull> hulls = {}) noexcept {
    switch (s.type) {
        case ShapeType::Sphere:
            return ray_vs_sphere(pos, s.radius, o, dir, tmax, t_out, n_out);
        case ShapeType::Box:
            return ray_vs_box(s.half_extents, pos, q, o, dir, tmax, t_out, n_out);
        case ShapeType::Capsule:
            return ray_vs_capsule(s.radius, s.half_height, pos, q, o, dir, tmax, t_out, n_out);
        case ShapeType::ConvexHull:
            return hull != nullptr && ray_vs_hull(*hull, pos, q, o, dir, tmax, t_out, n_out);
        case ShapeType::Compound: {
            if (compound == nullptr) {
                return false;
            }
            float best = tmax;
            bool hit = false;
            for (std::size_t i = 0; i < compound->child_count(); ++i) {
                const core::Vec3 cp = compound_child_world_pos(*compound, i, pos, q);
                const core::Quat cq = compound_child_world_orient(*compound, i, q);
                float t = 0.0f;
                core::Vec3 n{0.0f, 0.0f, 0.0f};
                // `best` as the child's bound: a farther child is rejected inside its own test.
                if (ray_vs_shape(compound->child_shape[i],
                                 cp,
                                 cq,
                                 o,
                                 dir,
                                 best,
                                 t,
                                 n,
                                 compound_child_hull(compound->child_shape[i], hulls)) &&
                    t < best) {
                    best = t;
                    n_out = n;
                    hit = true;
                }
            }
            if (hit) {
                t_out = best;
            }
            return hit;
        }
    }
    return false;
}

// Does a query sphere (center `c`, radius `sr`) overlap the posed shape? The exact test for
// overlap_sphere: distance from the sphere centre to the shape's nearest surface point ≤ sr, done
// in the shape's local frame (closest-point-on-box, closest-point-on-capsule-segment). A hull has
// no closed-form closest point, so it asks GJK — point-vs-hull distance is exactly GJK's output,
// and the query stays deterministic (GJK is a pure function of its supports). A compound overlaps
// iff ANY child does (fixed ascending scan; early-out on a pure OR cannot change the answer, so
// determinism of the result is untouched).
[[nodiscard]] inline bool sphere_vs_shape(core::Vec3 c,
                                          float sr,
                                          const ShapeDesc& s,
                                          core::Vec3 pos,
                                          const core::Quat& q,
                                          const ConvexHull* hull = nullptr,
                                          const CompoundShape* compound = nullptr,
                                          std::span<const ConvexHull> hulls = {}) noexcept {
    switch (s.type) {
        case ShapeType::Sphere: {
            const float rr = sr + s.radius;
            return core::length_squared(c - pos) <= rr * rr;
        }
        case ShapeType::Box: {
            const core::Vec3 lc = core::rotate(core::conjugate(q), c - pos);
            const core::Vec3 h = s.half_extents;
            const core::Vec3 closest{std::clamp(lc.x, -h.x, h.x),
                                     std::clamp(lc.y, -h.y, h.y),
                                     std::clamp(lc.z, -h.z, h.z)};
            return core::length_squared(lc - closest) <= sr * sr;
        }
        case ShapeType::Capsule: {
            const core::Vec3 lc = core::rotate(core::conjugate(q), c - pos);
            const float y = std::clamp(lc.y, -s.half_height, s.half_height);
            const core::Vec3 closest{0.0f, y, 0.0f};
            const float rr = sr + s.radius;
            return core::length_squared(lc - closest) <= rr * rr;
        }
        case ShapeType::ConvexHull: {
            if (hull == nullptr) {
                return false;
            }
            const ShapeSupport sup_h{&s, pos, q, hull};
            const SegmentSupport sup_c{c, c}; // a zero-length segment IS the point support
            const GjkResult g = gjk(sup_c, sup_h, c - pos);
            return g.overlapping || g.distance <= sr;
        }
        case ShapeType::Compound: {
            if (compound == nullptr) {
                return false;
            }
            for (std::size_t i = 0; i < compound->child_count(); ++i) {
                const core::Vec3 cp = compound_child_world_pos(*compound, i, pos, q);
                const core::Quat cq = compound_child_world_orient(*compound, i, q);
                if (sphere_vs_shape(c,
                                    sr,
                                    compound->child_shape[i],
                                    cp,
                                    cq,
                                    compound_child_hull(compound->child_shape[i], hulls))) {
                    return true; // children are never compounds — one level deep, as raycast
                }
            }
            return false;
        }
    }
    return false;
}

} // namespace rime::physics
