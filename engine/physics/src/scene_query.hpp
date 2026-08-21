// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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
//
// `child_out` (M8.3, RayHit::child): on a hit, receives the compound child index the ray pierced —
// 0 for every non-compound shape, matching the ContactEvent::child_a/child_b convention. Optional
// so the CCD/internal callers that don't care pay nothing.
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
                                       std::span<const ConvexHull> hulls = {},
                                       std::uint16_t* child_out = nullptr) noexcept {
    if (child_out != nullptr) {
        *child_out = 0; // non-compound shapes ARE child 0 (the ContactEvent convention)
    }
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
            std::size_t best_child = 0;
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
                    best_child = i;
                    hit = true;
                }
            }
            if (hit) {
                t_out = best;
                if (child_out != nullptr) {
                    // Child count is capped at 256 ≪ 65536, so the index always fits the 16 bits
                    // it travels in everywhere else (events, manifolds).
                    *child_out = static_cast<std::uint16_t>(best_child);
                }
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

// ── Shape casts: conservative advancement over GJK distance (m12.1) ──────────────────────────
//
// The idea in one line: measure how far apart the two shapes are, take a step that provably cannot
// carry the caster through anything, measure again, repeat. This is Mirtich's conservative
// advancement (by way of Bullet's convex cast), and it needs nothing from geometry beyond the
// support function GJK already speaks (docs/math/gjk-epa.md).
//
// What makes a step "provably safe" is the whole design, and this engine does NOT use the textbook
// `d / dot(dir, n)` — dividing the measured gap by its closing rate along the witness normal. That
// form is only as sound as `n`, and m12.1 measured a diagonal `n` turning one step into a 30x leap
// straight through a wall. What it steps by instead is GJK's SUPPORT-PLANE bound divided by the
// closing rate OF THAT SAME PLANE: bound and direction come from one statement about one plane
// (`GjkResult::lower_bound` and `plane_dir`), so a noisy direction can only tilt the plane and
// weaken the bound, never inflate the quotient. See THE ADVANCE inside the loop for the full
// argument, its two fallbacks, and the measurements behind each.
//
// Why not a swept-primitive routine per shape pair: there are five shape types, so pairs grow as
// n², each one its own algebra and its own bugs, and the destructible case (a hull) has no closed
// form at all. One loop over GJK covers every convex shape the engine has or will have.
namespace shape_cast_detail {

// How close counts as touching. A hair under a tenth of a millimetre: tight enough that a resting
// capsule is not visibly sunk into the floor, loose enough that the loop terminates in a handful
// of iterations rather than chasing float noise toward zero.
inline constexpr float kTouchTolerance = 5e-5f;

// The cap exists because conservative advancement converges geometrically but not finitely: a
// sweep that grazes a surface at a shallow angle takes ever-smaller steps forever. A head-on sweep
// finishes in one or two, and since the advance became a PROJECTED plane bound (see THE ADVANCE) an
// oblique one does too, so 64 is headroom rather than a working budget. It WAS a working budget,
// and an insufficient one: the radial bound it used to step by shrinks a graze's gap by only
// (1 - cos theta) per iteration, so a near-parallel sweep spent all 64 iterations and still stopped
// up to 3.35 m short of the wall (ROADMAP 2026-08-21).
//
// Running out is not a correctness problem. Exhaustion MEASURES the final position before
// reporting it (see the tail of the loop): a proven step cannot pass through anything, a rescue
// step that overshoots INTO the target is caught by the overlap branch on the very next iteration
// — and an overshoot taken on the LAST iteration, which has no next iteration, is caught by that
// final measurement and bisected back out the same way. Every exit therefore reports a position
// measured outside the target or bisected to its boundary.
inline constexpr int kMaxIterations = 64;

// Bisection steps used to recover from an overshoot (see the overlap branch below). 24 halvings
// resolve a kilometre-long sweep down to well under a tenth of a millimetre, and the loop exits
// early once the bracket is inside the touch tolerance, which is the usual case after two or three.
inline constexpr int kBisectIterations = 24;

// A floor under the advance, so a zero lower bound cannot stall the loop. Two orders of magnitude
// below the touch tolerance: small enough that it can never carry the caster through anything,
// large enough that the iteration cap is reached rather than the loop spinning in place. Since the
// distance rescue was added it is close to unreachable — a collapsed bound now falls through to the
// leashed measured gap rather than to this floor — but it stays as the guarantee that SOME progress
// is made in the case where every estimate above it is zero.
inline constexpr float kMinStep = 5e-7f;

// How firmly a contact normal must oppose the sweep to be believed. Small, because a legitimate
// grazing contact genuinely approaches perpendicular; the check is there to reject a normal that
// opposes the motion NOT AT ALL, which is geometrically impossible for a surface just run into.
inline constexpr float kNormalOpposesSweep = 1e-3f;

// When the PROVEN advance falls below this fraction of the measured gap, the proof machinery has
// stalled and the loop switches to the measured-distance rescue (see THE ADVANCE below). The
// measured failure is bimodal, which is what makes any threshold in the wide middle correct. A
// healthy iteration's plane bound is at least distance*(1 - kRelEps) - kSupportEps*|w| (the two
// terms of GJK's convergence test), and projecting can only enlarge it — near 1x the gap, though
// the absolute kSupportEps term erodes that as the gap approaches the touch tolerance against a
// large target, which is why the threshold sits at 1/16 rather than 1/2. A stalled iteration's
// bound is exactly zero: GJK's transverse noise, multiplied by a large target's support extent,
// drives the plane bound to nothing (ROADMAP 2026-08-21). Between "near 1x, eroded at the very
// end" and "exactly zero", 1/16 keeps an order of magnitude of margin on each side.
inline constexpr float kStallFraction = 1.0f / 16.0f;

// How far back along the sweep to step before MEASURING THE CONTACT NORMAL, and why that is
// necessary at all.
//
// At the touching configuration the two witness points are within kTouchTolerance of each other, so
// the direction between them is whatever float cancellation left behind. On a flat face it is worse
// than noisy: the witness may sit anywhere across that face without changing the distance, so the
// direction is barely determined even in exact arithmetic. Measuring where the shapes are provably
// apart makes the same direction well-conditioned by construction, rather than by hoping the
// iteration happened to land somewhere favourable.
//
// The distance is MEASURED, not guessed. Sweeping a 0.3 m sphere at walls of half-extent 1 m to
// 50 m from starts of 1 m to 500 m, the worst deviation of the reported normal from the true face
// normal was:
//
//     back-off        worst |n - n_true|
//     1 mm            0.99   (two configurations returned a diagonal axis)
//     1 cm            0.000000
//     5 cm            0.000000
//
// So 1 cm, scaled with coordinate magnitude because float error does: a fixed centimetre is
// generous at 50 m and meaningless at 10 km.
//
// This assertion has failed twice, which is why it is measured rather than reasoned about: first on
// x86-64 (a 50 m sweep reporting a normal 90 degrees off), then — after a fix that took the
// previous iteration's direction — on arm64 ONLY, because the two platforms round the advance
// differently and the last measured gap can land where the direction is already noise.
[[nodiscard]] inline float normal_back_off(float scale) noexcept {
    return std::max(1e-2f, 1e-4f * scale);
}

} // namespace shape_cast_detail

// One convex caster against one posed convex target. `dir` must be UNIT, so `t` is a world
// distance. On a hit, `n_out` is the outward normal of the TARGET at the touch (pointing back
// toward the caster) and `p_out` is the witness point on the target.
//
// `overlap_out` reports the case that must not be confused with a zero-distance touch: the shapes
// were already intersecting before the sweep began. A caller that treats "started inside a wall"
// as "stopped at the wall" produces a controller frozen in the geometry it is stuck in.
template <class TargetSupport>
[[nodiscard]] inline bool cast_convex_vs_convex(const ShapeDesc& caster_shape,
                                                core::Vec3 caster_origin,
                                                const core::Quat& caster_orient,
                                                const ConvexHull* caster_hull,
                                                const TargetSupport& target,
                                                core::Vec3 target_centre,
                                                core::Vec3 dir,
                                                float tmax,
                                                float& t_out,
                                                core::Vec3& n_out,
                                                core::Vec3& p_out,
                                                bool& overlap_out) {
    using namespace shape_cast_detail;

    // The caster, RE-POSED at distance `t` along the sweep — not "posed once at the origin, then
    // offset". The identity support_{A+v}(d) = support_A(d) + v is exact in real arithmetic and
    // ruinous in floats: at a 500 m start the offset support would compute a point near the wall as
    // (-500.3) + (499.5), two large numbers cancelling to ~0.8, so EVERY support point carried
    // ~5e-5 of noise — which is the touch tolerance itself, leaving the whole GJK loop working
    // inside its own error. Re-posing evaluates the support at the caster's ACTUAL current
    // position, which is small, and costs one vector add either way.
    //
    // Measured before this was fixed, sweeping a sphere at a wall from 500 m out: errors up to
    // 0.46 m, normals 90 degrees off, and — worst — walls dead ahead reported as clean misses.
    const auto posed_at = [&](float at_t) {
        return ShapeSupport{&caster_shape, caster_origin + dir * at_t, caster_orient, caster_hull};
    };

    // The contact normal is measured at a slightly RETRACTED position rather than at the touch
    // (see `normal_back_off` for the two platforms' worth of evidence behind that). One extra GJK
    // call, paid only on a hit, in exchange for a direction that is well-conditioned by
    // construction instead of by luck. `fallback` is used when even the retracted probe cannot
    // answer — the shapes still overlap there, which is the depenetration case.
    const auto measure_normal = [&](float hit_t, core::Vec3 fallback) -> core::Vec3 {
        const float scale = core::length(caster_origin) + std::fabs(hit_t);
        const float probe_t = hit_t - normal_back_off(scale);
        const ShapeSupport back = posed_at(probe_t);
        const GjkResult probe = gjk(back, target, back.pos - target_centre);
        if (!probe.overlapping) {
            // `closest` points from the target toward the caster and IS the target's outward
            // normal — no flip, and no differencing of witness points (GjkResult::closest explains
            // why that distinction earns a whole field).
            const float l = core::length(probe.closest);
            if (l > narrowphase_detail::kNormalEps) {
                const core::Vec3 n = probe.closest * (1.0f / l);

                // SANITY CHECK, and it is a geometric invariant rather than a fudge: a surface you
                // ran INTO must have a normal that opposes the motion that produced the contact.
                // dot(n, dir) < 0, always — a normal perpendicular to the sweep describes a surface
                // the caster travelled parallel to and could not have hit.
                //
                // It is here because GJK does not always converge to the right FEATURE on a large
                // box. A support function returns corners, and for a sweep aimed exactly at a face
                // all four of that face's corners are equally extreme; the tie is broken
                // deterministically, but which corner-simplex the iteration then settles on depends
                // on rounding. On arm64 it settles on one whose closest point is the box's EDGE
                // direction, and the reported normal comes back diagonal — (0, 0.707, 0.707) for an
                // axis-aligned sweep, which is exactly perpendicular to the travel.
                //
                // Rejecting that and falling back is strictly better than passing it on: -dir is
                // the correct answer for the face-on case, and for collide-and-slide "stop" is a
                // safe response where "slide sideways along a surface that is not there" is not.
                // The underlying convergence problem is NOT fixed by this — it is bounded. See the
                // deferred item in docs/ROADMAP.md.
                if (core::dot(n, dir) < -kNormalOpposesSweep) {
                    return n;
                }
            }
        }
        return fallback;
    };

    // The last position PROVEN to be outside the target — the lower bound the overshoot recovery
    // bisects from. Proven by observation, not by arithmetic.
    float safe_t = 0.0f;
    bool have_safe = false;

    // Recover from a measured overshoot: bisect between a position proven OUTSIDE and one proven
    // INSIDE until the bracket is within the touch tolerance, and return the outside end. Every
    // bound here is an observation — "GJK said overlapping" — rather than an arithmetic claim,
    // which is what makes "never stop inside the target" hold regardless of the distance's
    // accuracy. Shared by the in-loop overlap branch and the iteration-exhausted tail, so BOTH
    // exits give the same guarantee.
    const auto bisect_outside = [&](float lo, float hi) {
        for (int b = 0; b < kBisectIterations && (hi - lo) > kTouchTolerance; ++b) {
            const float mid = 0.5f * (lo + hi);
            const ShapeSupport probe = posed_at(mid);
            if (gjk(probe, target, probe.pos - target_centre).overlapping) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        return lo;
    };

    // The Lipschitz leash on GJK's distance. True distance is 1-LIPSCHITZ along the sweep — a
    // motion of length s changes it by at most s — so if the previous trusted value was a valid
    // upper bound, `trusted_prev + actual_step` is one too, and the tighter of it and the fresh
    // measurement remains one (induction; the base case is the first measurement, which is the
    // trust this loop cannot avoid). This is not defensive hedging: GJK's stall exits can return
    // distances that are not merely loose but WRONG — measured on this loop's own trace, 14.14 m
    // for a true gap of 2.8e-5 m, six orders of magnitude, at a near-touch pose against a large
    // box. A step that believes such a number leaps the whole target and reads the far side as a
    // clean miss. The leash is asymmetric on purpose: a falsely LARGE distance is the dangerous
    // lie (it moves the caster), while a falsely small one only slows the descent, so clamping
    // from above is safety and clamping from below is unnecessary.
    float trusted_dist = std::numeric_limits<float>::max();
    float last_advance = 0.0f;

    float t = 0.0f;
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        const ShapeSupport swept = posed_at(t);
        const GjkResult g = gjk(swept, target, swept.pos - target_centre);
        trusted_dist = std::min(g.distance, trusted_dist + last_advance);

        // The gap direction comes from the SEPARATING AXIS, never from `point_b - point_a`. The
        // two are equal in exact arithmetic and very different in floats once the target's support
        // vertices are large compared with the gap — a 100 m wall is enough. That matters far
        // beyond cosmetics: the advance below divides by `dot(dir, n)`, so a direction 20 degrees
        // off makes the step too LONG and conservative advancement stops being conservative.
        // Measured before this was fixed: a sweep at a 100 m wall stopped 0.93 m INSIDE it, and
        // another missed a wall dead ahead entirely.
        const float axis_len = core::length(g.closest);
        const core::Vec3 gap_dir = axis_len > narrowphase_detail::kNormalEps
                                       ? g.closest * (-1.0f / axis_len) // caster -> target
                                       : dir;

        // What this iteration alone can say about the direction — the fallback when the retracted
        // probe is unusable, and the whole answer for an initial overlap.
        const core::Vec3 witness_normal = gap_dir * -1.0f;

        if (g.overlapping) {
            if (!have_safe) {
                // Overlapping before moving at all: the caller's problem to fix (depenetration),
                // and a different instruction from "stopped at the surface". There is no gap
                // anywhere to measure, so the witnesses are all there is — a caller depenetrating
                // from here wants EPA's penetration axis, which this query deliberately does not
                // compute, and the flag is the signal to go and do that.
                t_out = 0.0f;
                overlap_out = true;
                n_out = witness_normal;
                p_out = g.point_b;
                return true;
            }
            // WE OVERSHOT — and the interesting part is that this is REACHABLE, which is why the
            // recovery exists rather than an assertion. Stepping by the reported gap should not be
            // able to pass through anything, because motion of length s changes a distance by at
            // most s. But GJK's distance is an UPPER bound: it terminates on a simplex, and the
            // closest point of a subset is never nearer than the closest point of the whole set, so
            // an early termination reports a gap slightly WIDER than the truth and the step is
            // slightly too long. On a large flat target that margin is enough to end up inside.
            //
            // So the answer is not to trust the number harder but to stop trusting it: bisect
            // between the last position proven OUTSIDE (safe_t) and this one proven INSIDE
            // (bisect_outside above). Paid only on the overshoot path.
            const float hit_t = bisect_outside(safe_t, t);
            t_out = hit_t; // the last position proven to be outside
            overlap_out = false;
            n_out = measure_normal(hit_t, witness_normal);
            p_out = g.point_b;
            return true;
        }

        if (g.distance <= kTouchTolerance) {
            t_out = t;
            overlap_out = false;
            n_out = measure_normal(t, witness_normal);
            p_out = g.point_b;
            return true;
        }

        have_safe = true;
        safe_t = t;

        // THE ADVANCE. Three ways forward, in descending order of what they can prove. The old
        // single rule — step by `g.lower_bound`, the radial support-plane bound — was sound but
        // insufficient twice over, measured on a 3,696-configuration sweep: the bound COLLAPSES TO
        // ZERO whenever GJK exits early (its transverse noise, multiplied by a large target's
        // support extent, swamps the axial term — ROADMAP 2026-08-21), so the loop crawled at
        // kMinStep; and even a HEALTHY radial bound shrinks a graze's gap by only (1 - cos theta)
        // per iteration, so at 85 degrees the 64-step budget ran out metres short. Worst measured
        // shortfall before this block: 3.35 m, with every 89-degree configuration failing.
        //
        // 1. THE PROJECTED PLANE BOUND — proven, and the reason grazes now converge. GJK's
        //    `lower_bound` is a support-plane statement: every point of the Minkowski difference
        //    lies at least that far along `g.plane_dir`. The sweep closes that plane at rate
        //    `closing = -dot(dir, plane_dir)`, so no contact can occur before an advance of
        //    lower_bound / closing (van den Bergen's ray-clip argument). Head-on this degrades to
        //    exactly the old radial step (closing = 1); at a graze it divides by a small closing
        //    rate and steps metres where the radial bound stepped millimetres.
        //
        //    m12.1 rejected the textbook `gap / dot(dir, n)` after measuring a diagonal `n` turn
        //    the quotient into a 30x leap through a wall, and THE PAIRING is what was missing: that
        //    form divides a distance by a direction the distance knows nothing about. Here the
        //    bound and the closing rate refer to the SAME plane, so a noisy `plane_dir` only tilts
        //    the plane away from the true gap and WEAKENS lower_bound — the quotient stays a
        //    proven under-estimate of the travel to contact, whatever the direction's quality.
        //
        //    A plane the sweep never closes (closing <= 0, bound > 0) separates the caster from
        //    the target for the WHOLE remaining sweep: a proven miss, returned right here rather
        //    than walked to tmax in 64 radial steps.
        //
        // 2. THE DISTANCE RESCUE — measured rather than proven, licensed by the recovery paths.
        //    When the proven step stalls (below kStallFraction of the gap), advance by the
        //    LEASHED distance (`trusted_dist`, never the raw measurement — see the leash above
        //    for the 14-metre lie that rule exists to stop). The leashed value is an UPPER bound,
        //    so it can overshoot — by exactly the slack GJK's early exit left in it — but
        //    overshooting INTO the target is the case the bisection above recovers by
        //    observation, and overshooting THROUGH it would need slack exceeding the target's
        //    chord along the sweep, when the leash bounds the slack by the gap's own scale and a
        //    graze whose chord is that short is below kTouchTolerance — not a hit this loop
        //    resolves even when it works.
        //
        // 3. THE FLOOR — kMinStep, so a zero gap estimate cannot stall the loop entirely; far
        //    under the touch tolerance, so it cannot itself overshoot anything meaningful.
        const float closing = -core::dot(dir, g.plane_dir);
        if (g.lower_bound > 0.0f && closing <= 0.0f) {
            return false; // a separating plane for the remaining sweep: a proven miss
        }
        const float projected = closing > 0.0f ? g.lower_bound / closing : 0.0f;
        const bool stalled = projected < trusted_dist * kStallFraction;
        const float step = std::max(stalled ? trusted_dist : projected, kMinStep);

        // A miss is never inferred from a number either: if the step would run past the end of the
        // sweep, CLAMP to the end and measure there, so "it never reaches" is a measurement at
        // tmax rather than an extrapolation. One extra GJK on the miss path, in exchange for a
        // false miss being impossible — and a false miss is a caster walking through a wall.
        const float next = t + step;
        if (next >= tmax) {
            if (t >= tmax) {
                return false; // measured at the far end, and still apart: a genuine clean sweep
            }
            last_advance = tmax - t; // the leash needs the ACTUAL advance, not the intended one
            t = tmax;
        } else {
            last_advance = step;
            t = next;
        }
    }

    // Iterations exhausted while still converging (a shallow graze). The final position must be
    // MEASURED before it is reported: a proven step lands outside by construction, but the RESCUE
    // may overshoot, and an overshoot taken on the last iteration has no next iteration to catch
    // it — returning it unmeasured would report a stop inside the target. One more GJK on this
    // rare path buys back the invariant every other exit keeps: a reported t is either measured
    // outside or bisected back to the boundary from the last position that was. (have_safe is
    // necessarily true here — 64 completed iterations each set it.) Then stop and call it a hit:
    // erring toward "stopped early" costs a fraction of a millimetre, while erring toward
    // "missed" lets the caster pass through a solid surface.
    const ShapeSupport last = posed_at(t);
    const GjkResult g_last = gjk(last, target, last.pos - target_centre);
    if (g_last.overlapping) {
        t = bisect_outside(safe_t, t);
    }
    t_out = t;
    overlap_out = false;
    n_out = measure_normal(t, dir * -1.0f);
    p_out = g_last.point_b;
    return true;
}

} // namespace rime::physics
