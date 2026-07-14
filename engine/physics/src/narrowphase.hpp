// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>
#include <cstdint>

#include "epa.hpp"
#include "gjk.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/contact.hpp"
#include "rime/physics/shape.hpp"
#include "support.hpp"

// The narrowphase (M7.3): turn one broadphase candidate pair into an exact contact manifold (or
// nothing). Route selection is the design decision worth reading (docs/design/physics.md,
// "Narrowphase"):
//
//  - Pairs involving a sphere or capsule take ANALYTIC FAST PATHS — spheres and capsule cores are
//    points and segments, whose closest-point problems have closed forms that are both cheaper and
//    more robust than any iterative method. The capsule paths use the shrunk-shape trick: collide
//    the core segment, then inflate the answer by the radius.
//  - Box–box (and, at M7.9, anything involving a convex hull) takes the GENERAL CONVEX path:
//    GJK for overlap (src/gjk.hpp), EPA for penetration normal/depth (src/epa.hpp), then
//    REFERENCE-FACE CLIPPING to turn that single deepest direction into a stable multi-point
//    manifold — a face-on-face contact needs up to four points or the stack wobbles.
//
// Every point carries a FEATURE ID: a hash of which shape features (box corner/face, capsule end,
// clip plane) generated it. The id is a pure function of contact topology, so the same physical
// contact reproduces the same id every frame — that is what lets the persistent manifold cache
// (ManifoldCacheEntry below) match points across frames and carry the solver's accumulated
// impulses forward (warm starting, consumed by M7.4). Only equality of ids ever matters; the
// values themselves are opaque.
//
// Conventions here match rime/physics/contact.hpp exactly: `normal` points from the FIRST shape
// argument (A) toward the SECOND (B), penetration >= 0, position = midpoint of the two surfaces.
//
// Private header (under src/), invisible above the PhysicsWorld seam.
namespace rime::physics {

// ------------------------------------------------------------------------------ feature ids ----
// boost::hash_combine-style fold (golden-ratio constant): cheap, good avalanche on small ints.
[[nodiscard]] constexpr std::uint32_t feature_combine(std::uint32_t h, std::uint32_t v) noexcept {
    return h ^ (v + 0x9E3779B9u + (h << 6) + (h >> 2));
}

// Per-routine seeds so ids from different collision routines can never alias structurally.
inline constexpr std::uint32_t kFeatSphereSphere = 0x53500001u;
inline constexpr std::uint32_t kFeatSphereBox = 0x53420001u;
inline constexpr std::uint32_t kFeatSphereCapsule = 0x53430001u;
inline constexpr std::uint32_t kFeatCapsuleCapsule = 0x43430001u;
inline constexpr std::uint32_t kFeatBoxCapsule = 0x42430001u;
inline constexpr std::uint32_t kFeatBoxBox = 0x42420001u;
inline constexpr std::uint32_t kFeatClipVertex = 0xC11B0001u;
inline constexpr std::uint32_t kFeatSpeculative = 0x5EC00001u; // M7.10 CCD speculative contact

namespace narrowphase_detail {

// Geometric tolerances (metre-scale, like GJK/EPA's — see the calibration note in gjk.hpp).
inline constexpr float kNormalEps = 1e-6f;        // below this, a direction is degenerate
inline constexpr float kParallelEps = 1e-6f;      // sin^2 of the capsule-axes "parallel" cone
inline constexpr float kRegionEps = 1e-4f;        // box-surface feature quantization slop (metres)
inline constexpr float kKeepEps = 1e-5f;          // clip keeps points this far above the ref face
inline constexpr float kFaceTieEps = 1e-4f;       // reference-face tie bias (prefer A: stable ids)
inline constexpr float kSpeculativeSlop = 0.005f; // CCD: fp buffer on "will they touch this step?"

// Append one contact point; silently drops beyond four (callers that can produce more reduce
// first — this is only a guard).
inline void add_point(Manifold& m, core::Vec3 mid, float pen, std::uint32_t id) noexcept {
    if (m.count >= 4) {
        return;
    }
    ContactPoint& p = m.points[m.count];
    p.position = mid;
    p.penetration = pen > 0.0f ? pen : 0.0f;
    p.feature_id = id;
    p.normal_impulse = 0.0f;
    p.tangent_impulse = 0.0f;
    ++m.count;
}

// A deterministic unit vector perpendicular to `v` — the fallback contact normal for degenerate
// configurations (concentric spheres, crossing capsule axes) where the true normal is undefined.
// Any perpendicular is "right"; determinism just demands the same one every time.
[[nodiscard]] inline core::Vec3 any_perpendicular(core::Vec3 v) noexcept {
    core::Vec3 p = core::cross(v, core::Vec3{1.0f, 0.0f, 0.0f});
    if (core::dot(p, p) <= 1e-12f) {
        p = core::cross(v, core::Vec3{0.0f, 1.0f, 0.0f});
    }
    const float len = core::length(p);
    return len > 1e-12f ? p * (1.0f / len) : core::Vec3{0.0f, 1.0f, 0.0f};
}

// Closest point on segment [p0, p1] to point c, as the clamped parameter t in [0, 1].
[[nodiscard]] inline float
closest_param_on_segment(core::Vec3 p0, core::Vec3 p1, core::Vec3 c) noexcept {
    const core::Vec3 d = p1 - p0;
    const float len2 = core::dot(d, d);
    if (len2 <= 1e-12f) {
        return 0.0f;
    }
    const float t = core::dot(c - p0, d) / len2;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Closest points between segments [p1,q1] and [p2,q2] (Ericson, Real-Time Collision Detection
// §5.1.9): minimize the quadratic |s·d1 - t·d2 + r|^2 over the unit square, clamping region by
// region. Handles every degeneracy (either or both segments being points) explicitly.
inline void closest_segment_segment(core::Vec3 p1,
                                    core::Vec3 q1,
                                    core::Vec3 p2,
                                    core::Vec3 q2,
                                    float& s,
                                    float& t,
                                    core::Vec3& c1,
                                    core::Vec3& c2) noexcept {
    const core::Vec3 d1 = q1 - p1;
    const core::Vec3 d2 = q2 - p2;
    const core::Vec3 r = p1 - p2;
    const float a = core::dot(d1, d1);
    const float e = core::dot(d2, d2);
    const float f = core::dot(d2, r);
    constexpr float kEps2 = 1e-12f;

    const auto clamp01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };

    if (a <= kEps2 && e <= kEps2) {
        s = t = 0.0f;
    } else if (a <= kEps2) {
        s = 0.0f;
        t = clamp01(f / e);
    } else {
        const float c = core::dot(d1, r);
        if (e <= kEps2) {
            t = 0.0f;
            s = clamp01(-c / a);
        } else {
            const float b = core::dot(d1, d2);
            const float denom = a * e - b * b; // >= 0; == 0 iff parallel
            s = denom > 0.0f ? clamp01((b * f - c * e) / denom) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = clamp01(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clamp01((b - c) / a);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

// Which end of a segment a parameter/point sits on: 0 = interior, 1 = the p0 end, 2 = the p1 end.
// Feeds feature ids: a contact that slides off a capsule's cylinder onto its cap is a different
// feature and must get a different id.
[[nodiscard]] inline std::uint32_t segment_end_tag(float t) noexcept {
    if (t <= 0.0f) {
        return 1u;
    }
    if (t >= 1.0f) {
        return 2u;
    }
    return 0u;
}

[[nodiscard]] inline std::uint32_t
segment_end_tag(core::Vec3 p, core::Vec3 p0, core::Vec3 p1) noexcept {
    const core::Vec3 d0 = p - p0;
    if (core::dot(d0, d0) <= 1e-10f) {
        return 1u;
    }
    const core::Vec3 d1 = p - p1;
    if (core::dot(d1, d1) <= 1e-10f) {
        return 2u;
    }
    return 0u;
}

// ------------------------------------------------------------------------------- boxes ---------
// A posed box unpacked for repeated local<->world work: unit world axes + half extents. Local
// coordinates of a world point are the dots against the axes; a corner/face is named by which
// side of each axis it sits on.
struct WorldBox {
    core::Vec3 center;
    core::Vec3 axis[3];
    float half[3];
};

[[nodiscard]] inline WorldBox
make_world_box(const ShapeDesc& s, core::Vec3 pos, const core::Quat& q) noexcept {
    WorldBox b;
    b.center = pos;
    b.axis[0] = core::rotate(q, core::Vec3{1.0f, 0.0f, 0.0f});
    b.axis[1] = core::rotate(q, core::Vec3{0.0f, 1.0f, 0.0f});
    b.axis[2] = core::rotate(q, core::Vec3{0.0f, 0.0f, 1.0f});
    b.half[0] = s.half_extents.x;
    b.half[1] = s.half_extents.y;
    b.half[2] = s.half_extents.z;
    return b;
}

[[nodiscard]] inline core::Vec3 box_local_point(const WorldBox& b, core::Vec3 world) noexcept {
    const core::Vec3 d = world - b.center;
    return core::Vec3{core::dot(d, b.axis[0]), core::dot(d, b.axis[1]), core::dot(d, b.axis[2])};
}

[[nodiscard]] inline core::Vec3 box_world_point(const WorldBox& b, core::Vec3 local) noexcept {
    return b.center + b.axis[0] * local.x + b.axis[1] * local.y + b.axis[2] * local.z;
}

// Stable feature codes for a box:
//  - face id: axis*2 + (positive side), 0..5;
//  - corner id: bit per axis on the positive side, 0..7;
//  - region code of a surface point: per axis, 0/1/2 for min-side/interior/max-side (with a slop
//    of kRegionEps, so fp noise on a face does not flip the code), packed base-3 -> 0..26.
[[nodiscard]] inline std::uint32_t box_region_code(const WorldBox& b, core::Vec3 world) noexcept {
    const core::Vec3 l = box_local_point(b, world);
    const float lv[3] = {l.x, l.y, l.z};
    std::uint32_t code = 0;
    for (int i = 0; i < 3; ++i) {
        std::uint32_t r = 1;
        if (lv[i] >= b.half[i] - kRegionEps) {
            r = 2;
        } else if (lv[i] <= -b.half[i] + kRegionEps) {
            r = 0;
        }
        code = code * 3u + r;
    }
    return code;
}

struct BoxFace {
    core::Vec3 normal;          // world, outward
    core::Vec3 verts[4];        // world corners, deterministic winding
    std::uint32_t corner_id[4]; // box corner ids (0..7)
    std::uint32_t id;           // face id (0..5)
};

[[nodiscard]] inline BoxFace box_face(const WorldBox& b, int axis, float sign) noexcept {
    const int i = (axis + 1) % 3;
    const int j = (axis + 2) % 3;
    const core::Vec3 face_center = b.center + b.axis[axis] * (sign * b.half[axis]);
    const core::Vec3 ei = b.axis[i] * b.half[i];
    const core::Vec3 ej = b.axis[j] * b.half[j];
    // Fixed corner order (-i,-j), (+i,-j), (+i,+j), (-i,+j): the winding itself is irrelevant to
    // clipping, but a FIXED order keeps clip output — and with it feature ids — deterministic.
    static constexpr float kSigns[4][2] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    BoxFace f;
    f.normal = b.axis[axis] * sign;
    f.id = static_cast<std::uint32_t>(axis * 2 + (sign > 0.0f ? 1 : 0));
    for (int k = 0; k < 4; ++k) {
        f.verts[k] = face_center + ei * kSigns[k][0] + ej * kSigns[k][1];
        std::uint32_t corner = 0;
        if (sign > 0.0f) {
            corner |= 1u << axis;
        }
        if (kSigns[k][0] > 0.0f) {
            corner |= 1u << i;
        }
        if (kSigns[k][1] > 0.0f) {
            corner |= 1u << j;
        }
        f.corner_id[k] = corner;
    }
    return f;
}

// The face of `b` whose outward normal is most aligned with `dir`. Ties resolve to the lowest
// face id — a fixed scan order, so the answer is a pure function of the inputs.
[[nodiscard]] inline BoxFace
most_aligned_face(const WorldBox& b, core::Vec3 dir, float* alignment = nullptr) noexcept {
    int best_axis = 0;
    float best_sign = 1.0f;
    float best = -2.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float d = core::dot(b.axis[axis], dir);
        for (const float sign : {-1.0f, 1.0f}) {
            const float a = sign * d;
            if (a > best) {
                best = a;
                best_axis = axis;
                best_sign = sign;
            }
        }
    }
    if (alignment != nullptr) {
        *alignment = best;
    }
    return box_face(b, best_axis, best_sign);
}

// A polygon vertex riding through Sutherland–Hodgman with its feature tag. Original vertices
// carry their box-corner id; vertices born at a clip carry a hash of (the two parent tags, the
// clipping plane) — the "edge cut by plane" feature, stable as long as the same edge crosses the
// same plane.
struct ClipVertex {
    core::Vec3 p;
    std::uint32_t tag;
};

// Clip a convex polygon against one half-space dot(p, n) <= off (Sutherland–Hodgman step). A
// convex clip adds at most one vertex net, so 4 input verts through 4 planes stay <= 8; the
// buffers are sized 12 for headroom and the guard just drops beyond that (never hit for boxes).
inline int clip_against_plane(const ClipVertex* in,
                              int in_count,
                              core::Vec3 n,
                              float off,
                              std::uint32_t plane_tag,
                              ClipVertex* out) noexcept {
    int out_count = 0;
    const auto push = [&](const ClipVertex& v) {
        if (out_count < 12) {
            out[out_count++] = v;
        }
    };
    for (int i = 0; i < in_count; ++i) {
        const ClipVertex& cur = in[i];
        const ClipVertex& nxt = in[(i + 1) % in_count];
        const float dc = core::dot(cur.p, n) - off;
        const float dn = core::dot(nxt.p, n) - off;
        if (dc <= 0.0f) {
            push(cur);
        }
        if ((dc <= 0.0f) != (dn <= 0.0f)) {
            const float t = dc / (dc - dn); // denominators differ in sign, so never zero
            ClipVertex born;
            born.p = cur.p + (nxt.p - cur.p) * t;
            born.tag = feature_combine(
                feature_combine(feature_combine(kFeatClipVertex, plane_tag), cur.tag), nxt.tag);
            push(born);
        }
    }
    return out_count;
}

// Reduce a >4-point manifold to the 4 points a solver actually needs: the deepest point (never
// drop the worst violation), the farthest point from it (contact patch extent), then twice the
// point adding the most spread (largest total triangle area against the already-chosen points).
// Greedy and order-deterministic (ties keep the lowest index), which matters: reduction picks
// FEATURES, and the cache matches on them next frame.
inline void reduce_manifold(Manifold& m,
                            const ClipVertex* verts,
                            const float* pens,
                            int count,
                            core::Vec3 mid_offset_dir,
                            const float* mid_dist) noexcept {
    int chosen[4] = {-1, -1, -1, -1};

    int deepest = 0;
    for (int i = 1; i < count; ++i) {
        if (pens[i] > pens[deepest]) {
            deepest = i;
        }
    }
    chosen[0] = deepest;

    const auto taken = [&](int i) {
        return i == chosen[0] || i == chosen[1] || i == chosen[2] || i == chosen[3];
    };

    float best_d2 = -1.0f;
    for (int i = 0; i < count; ++i) {
        if (taken(i)) {
            continue;
        }
        const core::Vec3 d = verts[i].p - verts[chosen[0]].p;
        const float d2 = core::dot(d, d);
        if (d2 > best_d2) {
            best_d2 = d2;
            chosen[1] = i;
        }
    }

    float best_area = -1.0f;
    for (int i = 0; i < count; ++i) {
        if (taken(i)) {
            continue;
        }
        const core::Vec3 c =
            core::cross(verts[chosen[1]].p - verts[chosen[0]].p, verts[i].p - verts[chosen[0]].p);
        const float area = core::dot(c, c);
        if (area > best_area) {
            best_area = area;
            chosen[2] = i;
        }
    }

    float best_spread = -1.0f;
    for (int i = 0; i < count; ++i) {
        if (taken(i)) {
            continue;
        }
        float spread = 0.0f;
        for (int e = 0; e < 3; ++e) {
            const core::Vec3 a = verts[chosen[e]].p;
            const core::Vec3 b = verts[chosen[(e + 1) % 3]].p;
            const core::Vec3 c = core::cross(b - a, verts[i].p - a);
            spread += std::sqrt(core::dot(c, c));
        }
        if (spread > best_spread) {
            best_spread = spread;
            chosen[3] = i;
        }
    }

    for (const int idx : chosen) {
        if (idx < 0) {
            continue;
        }
        add_point(
            m, verts[idx].p + mid_offset_dir * (mid_dist[idx] * 0.5f), pens[idx], verts[idx].tag);
    }
}

} // namespace narrowphase_detail

// ------------------------------------------------------------------------- pair routines -------
// Each routine takes shape A first, shape B second and fills normal (unit, A->B), points and
// count; body ids are the caller's business. Returns true iff a contact exists.

[[nodiscard]] inline bool
collide_sphere_sphere(core::Vec3 pa, float ra, core::Vec3 pb, float rb, Manifold& m) noexcept {
    using namespace narrowphase_detail;
    const core::Vec3 delta = pb - pa;
    const float d2 = core::dot(delta, delta);
    const float rsum = ra + rb;
    if (d2 >= rsum * rsum) {
        return false;
    }
    const float d = std::sqrt(d2);
    // Concentric centres leave the normal undefined; any direction separates. +Y, always.
    const core::Vec3 n = d > kNormalEps ? delta * (1.0f / d) : core::Vec3{0.0f, 1.0f, 0.0f};
    m.normal = n;
    const core::Vec3 surf_a = pa + n * ra;
    const core::Vec3 surf_b = pb - n * rb;
    add_point(m, (surf_a + surf_b) * 0.5f, rsum - d, kFeatSphereSphere);
    return true;
}

// Sphere A vs box B. Outside the box: closest point by per-axis clamp of the centre into the box
// (the box IS an axis product of intervals, so the clamp is exact). Inside: push out through the
// nearest face. The region code (which face/edge/corner of the box carries the contact) is the
// feature id.
[[nodiscard]] inline bool collide_sphere_box(core::Vec3 c,
                                             float r,
                                             const narrowphase_detail::WorldBox& b,
                                             Manifold& m) noexcept {
    using namespace narrowphase_detail;
    const core::Vec3 l = box_local_point(b, c);
    const float lv[3] = {l.x, l.y, l.z};
    float cl[3];
    bool inside = true;
    for (int i = 0; i < 3; ++i) {
        cl[i] = lv[i];
        if (cl[i] < -b.half[i]) {
            cl[i] = -b.half[i];
            inside = false;
        } else if (cl[i] > b.half[i]) {
            cl[i] = b.half[i];
            inside = false;
        }
    }

    if (!inside) {
        const core::Vec3 delta{lv[0] - cl[0], lv[1] - cl[1], lv[2] - cl[2]}; // box -> sphere, local
        const float d2 = core::dot(delta, delta);
        if (d2 >= r * r) {
            return false;
        }
        const float d = std::sqrt(d2);
        const core::Vec3 out_local = delta * (1.0f / d);
        // World direction box->sphere; the manifold normal is A->B = sphere->box = the negation.
        const core::Vec3 out_world =
            b.axis[0] * out_local.x + b.axis[1] * out_local.y + b.axis[2] * out_local.z;
        const core::Vec3 n = -out_world;
        m.normal = n;
        const core::Vec3 box_surf = box_world_point(b, core::Vec3{cl[0], cl[1], cl[2]});
        const core::Vec3 sphere_surf = c + n * r;
        std::uint32_t region = 0;
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t reg = lv[i] < -b.half[i] ? 0u : (lv[i] > b.half[i] ? 2u : 1u);
            region = region * 3u + reg;
        }
        add_point(
            m, (sphere_surf + box_surf) * 0.5f, r - d, feature_combine(kFeatSphereBox, region));
        return true;
    }

    // Centre inside the box: the closest exit is through the face with the smallest remaining
    // distance; penetration is that core distance plus the radius.
    int axis = 0;
    float sign = 1.0f;
    float min_d = b.half[0] - lv[0];
    for (int i = 0; i < 3; ++i) {
        const float dp = b.half[i] - lv[i];
        if (dp < min_d) {
            min_d = dp;
            axis = i;
            sign = 1.0f;
        }
        const float dn = lv[i] + b.half[i];
        if (dn < min_d) {
            min_d = dn;
            axis = i;
            sign = -1.0f;
        }
    }
    const core::Vec3 exit_world = b.axis[axis] * sign; // box -> sphere exit direction
    const core::Vec3 n = -exit_world;                  // A->B = sphere->box
    m.normal = n;
    core::Vec3 face_local{lv[0], lv[1], lv[2]};
    if (axis == 0) {
        face_local.x = sign * b.half[0];
    } else if (axis == 1) {
        face_local.y = sign * b.half[1];
    } else {
        face_local.z = sign * b.half[2];
    }
    const core::Vec3 box_surf = box_world_point(b, face_local);
    const core::Vec3 sphere_surf = c + n * r;
    const std::uint32_t face_id = static_cast<std::uint32_t>(axis * 2 + (sign > 0.0f ? 1 : 0));
    add_point(m,
              (sphere_surf + box_surf) * 0.5f,
              r + min_d,
              feature_combine(kFeatSphereBox, 27u + face_id));
    return true;
}

// Sphere A vs capsule B: closest point on the capsule's core segment to the sphere centre, then
// sphere-vs-sphere against a virtual sphere of the capsule's radius sitting there.
[[nodiscard]] inline bool collide_sphere_capsule(core::Vec3 c,
                                                 float rs,
                                                 core::Vec3 p0,
                                                 core::Vec3 p1,
                                                 float rc,
                                                 Manifold& m) noexcept {
    using namespace narrowphase_detail;
    const float t = closest_param_on_segment(p0, p1, c);
    const core::Vec3 q = p0 + (p1 - p0) * t;
    const core::Vec3 delta = q - c;
    const float d2 = core::dot(delta, delta);
    const float rsum = rs + rc;
    if (d2 >= rsum * rsum) {
        return false;
    }
    const float d = std::sqrt(d2);
    // Sphere centre exactly on the axis: separate sideways, perpendicular to the axis.
    const core::Vec3 n = d > kNormalEps ? delta * (1.0f / d) : any_perpendicular(p1 - p0);
    m.normal = n; // sphere -> capsule = A -> B
    const core::Vec3 surf_a = c + n * rs;
    const core::Vec3 surf_b = q - n * rc;
    add_point(m,
              (surf_a + surf_b) * 0.5f,
              rsum - d,
              feature_combine(kFeatSphereCapsule, segment_end_tag(t)));
    return true;
}

// Capsule A vs capsule B: closest points between the core segments, inflated by the radii. Two
// NEARLY PARALLEL side-by-side capsules are the special case worth extra code: the single
// closest point is numerically arbitrary along the overlap span and a one-point manifold lets a
// lying capsule seesaw — so emit the two ends of the overlapped span instead (the M7.4 solver
// then holds it flat, and the two ids are stable while the capsules stay side by side).
[[nodiscard]] inline bool collide_capsule_capsule(core::Vec3 a0,
                                                  core::Vec3 a1,
                                                  float ra,
                                                  core::Vec3 b0,
                                                  core::Vec3 b1,
                                                  float rb,
                                                  Manifold& m) noexcept {
    using namespace narrowphase_detail;
    const core::Vec3 da = a1 - a0;
    const core::Vec3 db = b1 - b0;
    const float la2 = core::dot(da, da);
    const float lb2 = core::dot(db, db);
    const float rsum = ra + rb;

    const core::Vec3 axb = core::cross(da, db);
    const bool parallel =
        la2 > 1e-12f && lb2 > 1e-12f && core::dot(axb, axb) <= kParallelEps * la2 * lb2;

    if (parallel) {
        // Project B's endpoints onto A's axis (in A-parameter units) and intersect the spans.
        const float t0 = core::dot(b0 - a0, da) / la2;
        const float t1 = core::dot(b1 - a0, da) / la2;
        const float lo = std::fmax(0.0f, std::fmin(t0, t1));
        const float hi = std::fmin(1.0f, std::fmax(t0, t1));
        if (hi - lo > 1e-6f) {
            // The radial offset between the parallel lines is constant along the span.
            const core::Vec3 perp = (b0 - a0) - da * t0;
            const float d2 = core::dot(perp, perp);
            if (d2 >= rsum * rsum) {
                return false;
            }
            const float d = std::sqrt(d2);
            const core::Vec3 n = d > kNormalEps ? perp * (1.0f / d) : any_perpendicular(da);
            m.normal = n; // A -> B
            const float pen = rsum - d;
            const float span[2] = {lo, hi};
            for (int k = 0; k < 2; ++k) {
                const core::Vec3 pa = a0 + da * span[k];
                const core::Vec3 pb = pa + perp;
                const core::Vec3 surf_a = pa + n * ra;
                const core::Vec3 surf_b = pb - n * rb;
                add_point(
                    m,
                    (surf_a + surf_b) * 0.5f,
                    pen,
                    feature_combine(kFeatCapsuleCapsule, 16u + static_cast<std::uint32_t>(k)));
            }
            return true;
        }
        // Parallel but end-to-end (no span overlap): the generic single point below handles it.
    }

    float s = 0.0f;
    float t = 0.0f;
    core::Vec3 c1;
    core::Vec3 c2;
    closest_segment_segment(a0, a1, b0, b1, s, t, c1, c2);
    const core::Vec3 delta = c2 - c1;
    const float d2 = core::dot(delta, delta);
    if (d2 >= rsum * rsum) {
        return false;
    }
    const float d = std::sqrt(d2);
    core::Vec3 n;
    if (d > kNormalEps) {
        n = delta * (1.0f / d);
    } else if (core::dot(axb, axb) > 1e-12f) {
        // Crossing axes: the common perpendicular is the cross direction (sign as computed — a
        // pure function of the inputs, hence deterministic).
        n = core::normalize(axb);
    } else {
        n = any_perpendicular(da);
    }
    m.normal = n;
    const core::Vec3 surf_a = c1 + n * ra;
    const core::Vec3 surf_b = c2 - n * rb;
    add_point(m,
              (surf_a + surf_b) * 0.5f,
              rsum - d,
              feature_combine(feature_combine(kFeatCapsuleCapsule, segment_end_tag(s)),
                              segment_end_tag(t)));
    return true;
}

// Box A vs capsule B — the shrunk-shape path: GJK measures the distance between the box and the
// capsule's CORE SEGMENT (both exact convex sets); a contact exists when it is under the capsule
// radius. If the core itself penetrates the box (deep overlap), EPA on the same pair supplies
// direction and core depth, and the radius is added on. One contact point v1: a capsule lying
// flat on a box face would prefer two — deferred, TODO(M7.9) alongside the convex-hull work.
[[nodiscard]] inline bool collide_box_capsule(const ShapeDesc& box_shape,
                                              core::Vec3 box_pos,
                                              const core::Quat& box_q,
                                              core::Vec3 p0,
                                              core::Vec3 p1,
                                              float r,
                                              Manifold& m) {
    using namespace narrowphase_detail;
    const WorldBox b = make_world_box(box_shape, box_pos, box_q);
    const ShapeSupport sup_box{&box_shape, box_pos, box_q};
    const SegmentSupport sup_seg{p0, p1};
    const GjkResult g = gjk(sup_box, sup_seg, box_pos - (p0 + p1) * 0.5f);

    if (!g.overlapping && g.distance > kNormalEps) {
        if (g.distance >= r) {
            return false;
        }
        const core::Vec3 n = (g.point_b - g.point_a) * (1.0f / g.distance); // box -> core = A -> B
        m.normal = n;
        const core::Vec3 surf_b = g.point_b - n * r;
        add_point(m,
                  (g.point_a + surf_b) * 0.5f,
                  r - g.distance,
                  feature_combine(feature_combine(kFeatBoxCapsule, box_region_code(b, g.point_a)),
                                  segment_end_tag(g.point_b, p0, p1)));
        return true;
    }

    // Core overlaps (or touches) the box: EPA for the core's own depth and exit direction.
    const EpaResult e = epa(sup_box, sup_seg, g.simplex, g.simplex_count);
    if (!e.valid) {
        return false; // numerically flat difference — drop this tick (documented posture)
    }
    const core::Vec3 n = e.normal; // pushes B (the core) out of A: A -> B by our EPA convention
    m.normal = n;
    const core::Vec3 surf_b = e.point_b - n * r;
    add_point(m,
              (e.point_a + surf_b) * 0.5f,
              e.depth + r,
              feature_combine(feature_combine(kFeatBoxCapsule, 32u + box_region_code(b, e.point_a)),
                              segment_end_tag(e.point_b, p0, p1)));
    return true;
}

// Box A vs box B — the general convex pipeline, end to end:
//   GJK: overlap? (no -> done)
//   EPA: penetration direction (the minimum translation) and depth
//   clipping: turn that one direction into a full contact PATCH.
// The patch step is what keeps stacks stable: pick the REFERENCE face (the face most aligned
// with the contact normal — ties biased to A so the choice, and with it every feature id, cannot
// flip between frames on fp noise), clip the other box's most anti-parallel (INCIDENT) face
// against the reference face's four side planes (Sutherland–Hodgman), keep what lies below the
// reference plane, and reduce to <= 4 spread points. Each survivor's id encodes its birth:
// either an incident-face corner or an (edge x side-plane) cut.
[[nodiscard]] inline bool collide_box_box(const ShapeDesc& sa,
                                          core::Vec3 pa,
                                          const core::Quat& qa,
                                          const ShapeDesc& sb,
                                          core::Vec3 pb,
                                          const core::Quat& qb,
                                          Manifold& m) {
    using namespace narrowphase_detail;
    const ShapeSupport sup_a{&sa, pa, qa};
    const ShapeSupport sup_b{&sb, pb, qb};
    const GjkResult g = gjk(sup_a, sup_b, pa - pb);
    if (!g.overlapping) {
        return false;
    }
    const EpaResult e = epa(sup_a, sup_b, g.simplex, g.simplex_count);
    if (!e.valid) {
        return false; // numerically flat difference — drop this tick (documented posture)
    }

    const WorldBox wa = make_world_box(sa, pa, qa);
    const WorldBox wb = make_world_box(sb, pb, qb);

    float align_a = 0.0f;
    float align_b = 0.0f;
    const BoxFace fa = most_aligned_face(wa, e.normal, &align_a);
    const BoxFace fb = most_aligned_face(wb, -e.normal, &align_b);

    // Reference face choice, biased toward A on near-ties (kFaceTieEps): flipping reference
    // between frames would relabel every feature id and defeat warm starting.
    const bool ref_is_a = !(align_b > align_a + kFaceTieEps);
    const WorldBox& ref_box = ref_is_a ? wa : wb;
    const BoxFace& ref = ref_is_a ? fa : fb;
    const WorldBox& inc_box = ref_is_a ? wb : wa;
    const BoxFace inc = most_aligned_face(inc_box, -ref.normal);

    // The manifold normal is the reference face's normal oriented A->B. EPA's raw normal is the
    // exact minimum-translation direction, but it is a polytope-triangle normal and wobbles at
    // float precision frame to frame; snapping to the chosen face is the standard stabilization
    // (and identical to it whenever the contact really is face-driven). Depths are measured
    // against the same face plane, so normal and depths stay mutually consistent.
    m.normal = ref_is_a ? ref.normal : -ref.normal;

    const float ref_off = core::dot(ref.normal, ref.verts[0]);
    const std::uint32_t side_seed = feature_combine(
        feature_combine(kFeatBoxBox, ref_is_a ? 0xA1u : 0xB2u), feature_combine(ref.id, inc.id));

    ClipVertex poly_a[12];
    ClipVertex poly_b[12];
    int count = 4;
    for (int k = 0; k < 4; ++k) {
        poly_a[k] = ClipVertex{inc.verts[k], inc.corner_id[k]};
    }
    ClipVertex* cur = poly_a;
    ClipVertex* nxt = poly_b;

    // Clip against the four side planes; each is the plane of the adjacent box face, so its box
    // face id is the natural stable plane tag.
    const int ref_axis = static_cast<int>(ref.id / 2);
    for (int step = 0; step < 4 && count > 0; ++step) {
        const int axis = step < 2 ? (ref_axis + 1) % 3 : (ref_axis + 2) % 3;
        const float sign = (step % 2 == 0) ? -1.0f : 1.0f;
        const core::Vec3 n_side = ref_box.axis[axis] * sign;
        const float off = core::dot(n_side, ref_box.center) + ref_box.half[axis];
        const std::uint32_t plane_tag =
            static_cast<std::uint32_t>(axis * 2 + (sign > 0.0f ? 1 : 0));
        count =
            clip_against_plane(cur, count, n_side, off, feature_combine(side_seed, plane_tag), nxt);
        ClipVertex* tmp = cur;
        cur = nxt;
        nxt = tmp;
    }

    // Keep only points at/below the reference face (actually penetrating); measure each point's
    // depth against the reference plane and finalize its id with the manifold-wide seed.
    ClipVertex kept[12];
    float pens[12];
    float dists[12];
    int kept_count = 0;
    for (int i = 0; i < count; ++i) {
        const float dist = core::dot(ref.normal, cur[i].p) - ref_off; // <= 0 when penetrating
        if (dist > kKeepEps) {
            continue;
        }
        kept[kept_count] = cur[i];
        kept[kept_count].tag = feature_combine(side_seed, cur[i].tag);
        pens[kept_count] = -dist;
        dists[kept_count] = -dist; // distance to travel along -ref.normal to reach the plane
        ++kept_count;
    }

    if (kept_count == 0) {
        // Clipping starved (a genuinely edge-edge contact can slide the incident face past the
        // side planes): fall back to the single EPA witness point so the contact is not lost.
        add_point(m, (e.point_a + e.point_b) * 0.5f, e.depth, feature_combine(side_seed, 0xEDEEu));
        return true;
    }

    // Points sit on the incident face; the contact-surface midpoint is halfway back toward the
    // reference plane along its normal.
    if (kept_count <= 4) {
        for (int i = 0; i < kept_count; ++i) {
            add_point(m, kept[i].p + ref.normal * (dists[i] * 0.5f), pens[i], kept[i].tag);
        }
    } else {
        reduce_manifold(m, kept, pens, kept_count, ref.normal, dists);
    }
    return true;
}

// ------------------------------------------------------------------------------ dispatch -------
// Collide two posed shapes; fills normal/points/count (never the body ids). Pairs are
// canonicalized by ShapeType so each unordered combination has exactly one routine; when the
// arguments arrive in the other order the manifold is computed swapped and the normal flipped
// (positions, penetrations and feature ids are side-symmetric already).
[[nodiscard]] inline bool collide_shapes(const ShapeDesc& sa,
                                         core::Vec3 pa,
                                         const core::Quat& qa,
                                         const ShapeDesc& sb,
                                         core::Vec3 pb,
                                         const core::Quat& qb,
                                         Manifold& m) {
    m.count = 0;
    if (static_cast<std::uint8_t>(sa.type) > static_cast<std::uint8_t>(sb.type)) {
        if (!collide_shapes(sb, pb, qb, sa, pa, qa, m)) {
            return false;
        }
        m.normal = -m.normal;
        return true;
    }

    const auto capsule_ends = [](const ShapeDesc& s,
                                 core::Vec3 pos,
                                 const core::Quat& q,
                                 core::Vec3& p0,
                                 core::Vec3& p1) {
        const core::Vec3 axis = core::rotate(q, core::Vec3{0.0f, s.half_height, 0.0f});
        p0 = pos - axis;
        p1 = pos + axis;
    };

    switch (sa.type) {
        case ShapeType::Sphere:
            switch (sb.type) {
                case ShapeType::Sphere:
                    return collide_sphere_sphere(pa, sa.radius, pb, sb.radius, m);
                case ShapeType::Box:
                    return collide_sphere_box(
                        pa, sa.radius, narrowphase_detail::make_world_box(sb, pb, qb), m);
                case ShapeType::Capsule: {
                    core::Vec3 b0;
                    core::Vec3 b1;
                    capsule_ends(sb, pb, qb, b0, b1);
                    return collide_sphere_capsule(pa, sa.radius, b0, b1, sb.radius, m);
                }
            }
            break;
        case ShapeType::Box:
            switch (sb.type) {
                case ShapeType::Box:
                    return collide_box_box(sa, pa, qa, sb, pb, qb, m);
                case ShapeType::Capsule: {
                    core::Vec3 b0;
                    core::Vec3 b1;
                    capsule_ends(sb, pb, qb, b0, b1);
                    return collide_box_capsule(sa, pa, qa, b0, b1, sb.radius, m);
                }
                default:
                    break;
            }
            break;
        case ShapeType::Capsule: {
            core::Vec3 a0;
            core::Vec3 a1;
            core::Vec3 b0;
            core::Vec3 b1;
            capsule_ends(sa, pa, qa, a0, a1);
            capsule_ends(sb, pb, qb, b0, b1);
            return collide_capsule_capsule(a0, a1, sa.radius, b0, b1, sb.radius, m);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------- speculative (CCD) --
// A SPECULATIVE contact for continuous collision detection (M7.10). The exact narrowphase above
// only reports pairs that already overlap — so a body moving farther than its own thickness in one
// step can pass clean through a thin obstacle, never sampled overlapping. This closes that gap
// WITHOUT a time-of-impact rewind (ADR-0026): when two SEPARATED shapes are approaching fast enough
// to touch within this step, emit a one-point contact carrying the NEGATIVE penetration (the gap
// still to be closed). The solver then permits the pair to approach only far enough to touch and no
// further (its speculative bias) — the body stops AT the surface instead of through it.
//
// Shape-agnostic by construction: it runs on GJK's closest-point witnesses (point_a/point_b, which
// GJK already computes for a separated pair), so every convex primitive is handled by this one path
// with no per-type code. `va`/`vb` are the bodies' LINEAR velocities; angular approach is ignored
// on purpose (CCD for fast-SPINNING bodies is a deferred, conservative-advancement case —
// ADR-0026). Returns false (m left empty) when the pair overlaps (the exact path owns that), is not
// approaching, or is still too far to touch this step. `dt` must be > 0.
[[nodiscard]] inline bool collide_speculative(const ShapeDesc& sa,
                                              core::Vec3 pa,
                                              const core::Quat& qa,
                                              core::Vec3 va,
                                              const ShapeDesc& sb,
                                              core::Vec3 pb,
                                              const core::Quat& qb,
                                              core::Vec3 vb,
                                              float dt,
                                              Manifold& m) {
    m.count = 0;
    const ShapeSupport support_a{&sa, pa, qa};
    const ShapeSupport support_b{&sb, pb, qb};
    const GjkResult g = gjk(support_a, support_b, pa - pb);
    if (g.overlapping || g.distance <= narrowphase_detail::kNormalEps) {
        return false; // overlapping (the exact narrowphase owns it) or coincident — no gap to speak
                      // of
    }

    // Gap direction: from A's witness toward B's — the manifold normal (a → b), matching the
    // contact.hpp convention the exact routines also honour.
    const core::Vec3 delta = g.point_b - g.point_a;
    const float dlen = core::length(delta);
    if (dlen <= narrowphase_detail::kNormalEps) {
        return false;
    }
    const core::Vec3 n = delta / dlen;

    // Closing speed along the normal (linear only). A - B relative velocity dotted with n > 0 means
    // the surfaces are approaching; <= 0 means holding or separating, so no imminent contact.
    const float closing = core::dot(va - vb, n);
    if (closing <= 0.0f) {
        return false;
    }
    // Will they meet this step? The gap closes by closing·dt; a small slop absorbs fp noise so a
    // just-reaching contact is not missed. If still farther than that, defer — a later step (with
    // the pair now nearer, its swept broadphase bound still reporting it) will speculate then.
    if (g.distance > closing * dt + narrowphase_detail::kSpeculativeSlop) {
        return false;
    }

    // One point at the midpoint of the two witnesses, carrying the gap as a NEGATIVE penetration.
    // Written directly rather than through add_point(), which deliberately clamps penetration to >=
    // 0 for the exact paths; the speculative gap is the one place a negative value is meaningful.
    m.normal = n;
    ContactPoint& p = m.points[0];
    p.position = (g.point_a + g.point_b) * 0.5f;
    p.penetration = -g.distance;
    p.feature_id = kFeatSpeculative;
    p.normal_impulse = 0.0f;
    p.tangent_impulse = 0.0f;
    m.count = 1;
    return true;
}

// ------------------------------------------------------------------------ manifold cache -------
// One persistent-cache entry per body pair, keyed like the broadphase pair list
// ((slot_a << 32) | slot_b, slot_a < slot_b) and guarded by the ids' generations so a recycled
// slot can never inherit a dead pair's impulses. The cache stores only what warm starting needs:
// each point's feature id and accumulated impulses. Matching is feature-id equality — the whole
// reason the ids exist (contact.hpp). M7.3 shipped this machinery persisting zeros; since M7.4
// the step commits it AFTER the solve, so the floats are the solver's converged impulses.
struct ManifoldCacheEntry {
    std::uint64_t key = 0;
    std::uint32_t gen_a = 0;
    std::uint32_t gen_b = 0;
    std::uint8_t count = 0;
    std::uint32_t feature_id[4] = {};
    float normal_impulse[4] = {};
    float tangent_impulse[4] = {};
};

// Carry cached impulses into `m` for every point whose feature id matches; returns how many
// matched (the "persisted" statistic — and, from M7.4, the warm-start hit rate).
[[nodiscard]] inline std::uint32_t warm_start_from(const ManifoldCacheEntry& e,
                                                   Manifold& m) noexcept {
    std::uint32_t matched = 0;
    for (std::uint8_t i = 0; i < m.count; ++i) {
        for (std::uint8_t j = 0; j < e.count; ++j) {
            if (m.points[i].feature_id == e.feature_id[j]) {
                m.points[i].normal_impulse = e.normal_impulse[j];
                m.points[i].tangent_impulse = e.tangent_impulse[j];
                ++matched;
                break;
            }
        }
    }
    return matched;
}

[[nodiscard]] inline ManifoldCacheEntry make_cache_entry(std::uint64_t key,
                                                         std::uint32_t gen_a,
                                                         std::uint32_t gen_b,
                                                         const Manifold& m) noexcept {
    ManifoldCacheEntry e;
    e.key = key;
    e.gen_a = gen_a;
    e.gen_b = gen_b;
    e.count = m.count;
    for (std::uint8_t i = 0; i < m.count; ++i) {
        e.feature_id[i] = m.points[i].feature_id;
        e.normal_impulse[i] = m.points[i].normal_impulse;
        e.tangent_impulse[i] = m.points[i].tangent_impulse;
    }
    return e;
}

} // namespace rime::physics
