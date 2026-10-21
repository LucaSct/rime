// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "solver.hpp"

// SSE2 is the x86-64 baseline (every x86-64 CPU has it, and every compiler we build with defines
// one of these for it), so on those targets the wide path needs no build flag and no runtime
// dispatch. Elsewhere — arm64 today — the wide path is compiled out and the island takes the
// scalar sweep; see kWideVelocitySolveAvailable for why that is a deliberate choice and not an
// unfinished port.
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RIME_PHYSICS_WIDE_SSE2 1
#else
#define RIME_PHYSICS_WIDE_SSE2 0
#endif

// WAVEFRONT ORDER for one island's velocity sweep — finding the parallelism that is ALREADY in a
// Gauss–Seidel sweep, without changing a single bit of its answer.
//
// A Gauss–Seidel sweep looks irreducibly serial: constraint j reads the velocities every earlier
// constraint wrote. But it only reads the rows of ITS two bodies, so constraint j truly depends on
// just two earlier constraints — the last one before it that touched body a, and the last one that
// touched body b. Everything else in the sweep is invisible to it. Those "last writer" edges form a
// dependency DAG, and the sequential sweep is merely ONE topological order of it. Any other
// topological order performs exactly the same floating-point operations on exactly the same
// operands — every constraint still sees exactly the velocities it saw before — so the result is
// bit-identical. This is LEVEL SCHEDULING, the classic way to parallelise a sparse triangular solve
// (and "wavefront" Gauss–Seidel): give each constraint a level one deeper than the deepest earlier
// constraint it shares a body with. Then no two constraints on one level share a body, a whole
// level can be solved at once — on SIMD lanes, here — and running the levels in order keeps every
// dependency edge pointing the way the sequential sweep had it.
//
// What this is NOT, because the two are easy to confuse: it is not GRAPH COLOURING. Colouring also
// produces sets of body-disjoint constraints, but it is free to solve a later constraint before an
// earlier one it depends on — it picks a different Gauss–Seidel order, which changes convergence
// and every bit downstream (that is a quality decision, ADR-0026's "strictly sequential within an
// island"). Level order picks no new order: the sweep is the same sweep. Nor is it Jacobi: no
// constraint ever reads a stale velocity.
//
// Only DYNAMIC bodies create edges. A static or kinematic body is read-only during the solve (the
// M7.5 guard in apply_impulse), so two constraints on the same immovable anchor never communicate
// through it — which matters, because otherwise the ground would chain every contact resting on it
// into one serial line.
//
// Across the iteration boundary nothing more is needed: running all of sweep 1 (in level order)
// before any of sweep 2 is itself consistent with the sequential order, so the same per-sweep order
// serves every velocity iteration.
//
// MEASURED on 99-the-block's tail island (1348 constraints): 124 levels, i.e. ~11 independent
// constraints per level on average; packed four to a batch, 89 % of SIMD lanes carry real work.
// The fixed-world solver bench's 448-body pile: 3846 constraints in 162 levels, 94 % lane use.
namespace rime::physics {

// Assigns each constraint of one island its 0-based wavefront level and returns the level count.
//
// `body_depth` is caller-owned scratch indexed by dense body index; it must read 0 for every
// DYNAMIC body these constraints touch (the caller zeroes its island's bodies — islands own
// disjoint dynamic bodies, so concurrent islands can share one array without a race). Immovable
// bodies are never read or written here.
[[nodiscard]] inline std::uint32_t
assign_wavefront_levels(std::span<const ContactConstraint> constraints,
                        const float* inv_mass,
                        std::uint32_t* body_depth,
                        std::uint32_t* level_out) noexcept {
    std::uint32_t levels = 0;
    for (std::size_t j = 0; j < constraints.size(); ++j) {
        const std::uint32_t a = constraints[j].body_a;
        const std::uint32_t b = constraints[j].body_b;
        const bool dyn_a = inv_mass[a] > 0.0f;
        const bool dyn_b = inv_mass[b] > 0.0f;
        // depth = 1 + the deepest earlier constraint on either dynamic body (a body's depth is
        // the depth of the last constraint that wrote it).
        const std::uint32_t depth =
            1u + std::max(dyn_a ? body_depth[a] : 0u, dyn_b ? body_depth[b] : 0u);
        if (dyn_a) {
            body_depth[a] = depth;
        }
        if (dyn_b) {
            body_depth[b] = depth;
        }
        level_out[j] = depth - 1u;
        levels = std::max(levels, depth);
    }
    return levels;
}

// ------------------------------------------------------------------- the four-lane batch -------
// Four constraints from ONE level, laid out structure-of-arrays so that lane l of every field is
// constraint l's value: the solve then runs the four constraints' identical instruction streams
// side by side in one SSE register each. Everything the velocity iterations only READ — geometry,
// effective masses, biases, the bodies' inverse masses and world inverse inertias — is packed once
// per step; only the four accumulated impulses per point are written, and the body velocities are
// gathered from and scattered back to the world's arrays around each batch (they are the one thing
// that changes between levels).
//
// An unfilled lane (a level whose width is not a multiple of four) carries all-zero masks: it
// computes garbage nobody reads and stores nothing.
inline constexpr int kWideLanes = 4;

struct alignas(16) WideContactPoint {
    float r_a[3][kWideLanes];
    float r_b[3][kWideLanes];
    float neg_normal_mass[kWideLanes];     // −normal_mass: negating is exact, so pre-negated
    float neg_tangent_mass[2][kWideLanes]; // −tangent_mass, likewise
    float velocity_bias[kWideLanes];
    float normal_impulse[kWideLanes];
    float tangent_impulse[2][kWideLanes];
};

struct alignas(16) WideContactBatch {
    // All-ones / all-zero per lane, used as bit masks: "this lane's body a is dynamic" (so it is
    // updated and stored — the M7.5 read-only guard), and "this lane HAS a point k".
    std::uint32_t dyn_a[kWideLanes];
    std::uint32_t dyn_b[kWideLanes];
    std::uint32_t point_active[4][kWideLanes];
    std::uint32_t body_a[kWideLanes];
    std::uint32_t body_b[kWideLanes];
    std::uint32_t constraint[kWideLanes]; // index into the island's span (unpack writes back)
    float normal[3][kWideLanes];
    float tangent[2][3][kWideLanes];
    float friction[kWideLanes];
    float inv_mass_a[kWideLanes];
    float inv_mass_b[kWideLanes];
    float inverse_inertia_a[6][kWideLanes]; // SymMat3 xx, xy, xz, yy, yz, zz
    float inverse_inertia_b[6][kWideLanes];
    WideContactPoint points[4];
    // The two scalars LAST: every array above is exactly one 16-byte register wide, so with the
    // struct aligned each of them lands on a 16-byte boundary — which the aligned loads in
    // solve_velocities_wide require. A scalar in the middle would shift every array after it.
    std::uint32_t lanes = 0;      // filled lanes, 1..4 (they are always the first ones)
    std::int32_t point_count = 0; // the most points any lane has (0..4)
};

static_assert(offsetof(WideContactBatch, points) % 16 == 0 &&
                  offsetof(WideContactBatch, inverse_inertia_b) % 16 == 0 &&
                  offsetof(WideContactBatch, normal) % 16 == 0,
              "every lane array must start on a 16-byte boundary (aligned SSE loads)");

namespace wide {

// The smallest vocabulary the sweep needs, each operation the exact IEEE single-precision
// operation the scalar code performs on one lane. Nothing here may be "the same maths, fused": the
// whole proof of this path is that lane l executes, operation for operation, what solve_velocities
// executes for constraint l — so no FMA, no reciprocal estimate, no reassociation, and the select
// helpers reproduce std::max / std::clamp's comparison forms exactly (see max0 and clamp below).
#if RIME_PHYSICS_WIDE_SSE2
struct F4 {
    __m128 v;
};

struct M4 {
    __m128 v; // all-ones / all-zero lanes
};

[[nodiscard]] inline F4 load(const float* p) noexcept {
    return {_mm_load_ps(p)};
}

inline void store(float* p, F4 a) noexcept {
    _mm_store_ps(p, a.v);
}

[[nodiscard]] inline M4 load_mask(const std::uint32_t* p) noexcept {
    return {_mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i*>(p)))};
}

[[nodiscard]] inline F4 operator+(F4 a, F4 b) noexcept {
    return {_mm_add_ps(a.v, b.v)};
}

[[nodiscard]] inline F4 operator-(F4 a, F4 b) noexcept {
    return {_mm_sub_ps(a.v, b.v)};
}

[[nodiscard]] inline F4 operator*(F4 a, F4 b) noexcept {
    return {_mm_mul_ps(a.v, b.v)};
}

// Unary minus is a sign-bit flip in IEEE arithmetic, scalar or not, so XOR with −0.0 is exact.
[[nodiscard]] inline F4 operator-(F4 a) noexcept {
    return {_mm_xor_ps(a.v, _mm_set1_ps(-0.0f))};
}

[[nodiscard]] inline M4 less(F4 a, F4 b) noexcept {
    return {_mm_cmplt_ps(a.v, b.v)};
}

[[nodiscard]] inline M4 operator&(M4 a, M4 b) noexcept {
    return {_mm_and_ps(a.v, b.v)};
}

// m ? a : b, lane by lane, by bits — no arithmetic touches either operand.
[[nodiscard]] inline F4 select(M4 m, F4 a, F4 b) noexcept {
    return {_mm_or_ps(_mm_and_ps(m.v, a.v), _mm_andnot_ps(m.v, b.v))};
}

[[nodiscard]] inline F4 zero() noexcept {
    return {_mm_setzero_ps()};
}
#endif

} // namespace wide

// Whether this build can take the wide path at all. Off where there is no SSE2: a portable
// float[4] fallback would be correct on its own terms, but it could NOT promise what the SSE path
// promises. On arm64, Clang's default -ffp-contract=on fuses `a*b + c` inside one source
// expression of the SCALAR solver into an FMA, and an emulation that spells the same maths as
// separate statements would round differently — a deterministic but different answer, which is
// precisely the kind of change this file exists not to make. A NEON port has to be checked against
// that compiler's contraction of solver.hpp first; until then arm64 keeps the scalar sweep.
inline constexpr bool kWideVelocitySolveAvailable = RIME_PHYSICS_WIDE_SSE2 != 0;

// Islands smaller than this keep the scalar sweep. Both paths produce the same bits, so this is a
// pure cost threshold: below it, packing and the per-batch gather/scatter cannot be amortised.
inline constexpr std::size_t kWideMinConstraints = 32;

// Pack one island's constraints into level-ordered four-lane batches. `level` is
// assign_wavefront_levels' output, `level_start` scratch of (levels + 1) entries, `order` scratch
// of constraints.size() entries. Returns the batch count; `batches` must hold at least
// constraints.size() of them (one per constraint is the worst case: every level of width one).
[[nodiscard]] inline std::size_t pack_wide_batches(const SolverBodies& bodies,
                                                   std::span<const ContactConstraint> constraints,
                                                   const std::uint32_t* level,
                                                   std::uint32_t levels,
                                                   std::uint32_t* level_start,
                                                   std::uint32_t* order,
                                                   WideContactBatch* batches) noexcept {
    // A counting sort by level that keeps the original order inside each level (stable). The order
    // inside a level does not affect the answer — its constraints share no dynamic body — but a
    // stable sort makes the packing itself a pure function of the island, which is cheaper to
    // reason about than "any order works".
    std::fill(level_start, level_start + levels + 1, 0u);
    for (std::size_t j = 0; j < constraints.size(); ++j) {
        ++level_start[level[j] + 1];
    }
    for (std::uint32_t l = 0; l < levels; ++l) {
        level_start[l + 1] += level_start[l];
    }
    for (std::size_t j = 0; j < constraints.size(); ++j) {
        order[level_start[level[j]]++] = static_cast<std::uint32_t>(j);
    }
    // level_start[l] now holds the END of level l; the start of level l is the end of l − 1.

    std::size_t batch_count = 0;
    std::uint32_t begin = 0;
    for (std::uint32_t l = 0; l < levels; ++l) {
        const std::uint32_t end = level_start[l];
        for (std::uint32_t i = begin; i < end; i += kWideLanes) {
            WideContactBatch& w = batches[batch_count++];
            w = WideContactBatch{};
            w.lanes = std::min<std::uint32_t>(kWideLanes, end - i);
            for (std::uint32_t lane = 0; lane < kWideLanes; ++lane) {
                // An empty lane points at the batch's first constraint's bodies, so its gathers
                // read real, in-bounds memory; its masks stay zero, so it never stores.
                const std::uint32_t j = order[i + (lane < w.lanes ? lane : 0u)];
                const ContactConstraint& c = constraints[j];
                const bool filled = lane < w.lanes;
                w.constraint[lane] = j;
                w.body_a[lane] = c.body_a;
                w.body_b[lane] = c.body_b;
                const float ima = bodies.inv_mass[c.body_a];
                const float imb = bodies.inv_mass[c.body_b];
                w.dyn_a[lane] = filled && ima > 0.0f ? ~0u : 0u;
                w.dyn_b[lane] = filled && imb > 0.0f ? ~0u : 0u;
                w.inv_mass_a[lane] = ima;
                w.inv_mass_b[lane] = imb;
                const SymMat3& ia = bodies.inverse_inertia_world[c.body_a];
                const SymMat3& ib = bodies.inverse_inertia_world[c.body_b];
                const float ia6[6] = {ia.xx, ia.xy, ia.xz, ia.yy, ia.yz, ia.zz};
                const float ib6[6] = {ib.xx, ib.xy, ib.xz, ib.yy, ib.yz, ib.zz};
                for (int e = 0; e < 6; ++e) {
                    w.inverse_inertia_a[e][lane] = ia6[e];
                    w.inverse_inertia_b[e][lane] = ib6[e];
                }
                const float n[3] = {c.normal.x, c.normal.y, c.normal.z};
                const float t[2][3] = {{c.tangent[0].x, c.tangent[0].y, c.tangent[0].z},
                                       {c.tangent[1].x, c.tangent[1].y, c.tangent[1].z}};
                for (int d = 0; d < 3; ++d) {
                    w.normal[d][lane] = n[d];
                    w.tangent[0][d][lane] = t[0][d];
                    w.tangent[1][d][lane] = t[1][d];
                }
                w.friction[lane] = c.friction;
                if (filled) {
                    w.point_count = std::max<std::int32_t>(w.point_count, c.count);
                }
                for (int k = 0; k < 4; ++k) {
                    const bool active = filled && k < c.count;
                    w.point_active[k][lane] = active ? ~0u : 0u;
                    if (!active) {
                        continue;
                    }
                    const ContactPointConstraint& p = c.points[k];
                    WideContactPoint& wp = w.points[k];
                    const float ra[3] = {p.r_a.x, p.r_a.y, p.r_a.z};
                    const float rb[3] = {p.r_b.x, p.r_b.y, p.r_b.z};
                    for (int d = 0; d < 3; ++d) {
                        wp.r_a[d][lane] = ra[d];
                        wp.r_b[d][lane] = rb[d];
                    }
                    wp.neg_normal_mass[lane] = -p.normal_mass;
                    wp.neg_tangent_mass[0][lane] = -p.tangent_mass[0];
                    wp.neg_tangent_mass[1][lane] = -p.tangent_mass[1];
                    wp.velocity_bias[lane] = p.velocity_bias;
                    wp.normal_impulse[lane] = p.normal_impulse;
                    wp.tangent_impulse[0][lane] = p.tangent_impulse[0];
                    wp.tangent_impulse[1][lane] = p.tangent_impulse[1];
                }
            }
        }
        begin = end;
    }
    return batch_count;
}

// Write the converged accumulators back into the constraints, so store_impulses (and anything
// else downstream) sees exactly what the scalar sweep would have left there.
inline void unpack_wide_impulses(std::span<const WideContactBatch> batches,
                                 std::span<ContactConstraint> constraints) noexcept {
    for (const WideContactBatch& w : batches) {
        for (std::uint32_t lane = 0; lane < w.lanes; ++lane) {
            ContactConstraint& c = constraints[w.constraint[lane]];
            for (std::uint8_t k = 0; k < c.count; ++k) {
                const WideContactPoint& wp = w.points[k];
                c.points[k].normal_impulse = wp.normal_impulse[lane];
                c.points[k].tangent_impulse[0] = wp.tangent_impulse[0][lane];
                c.points[k].tangent_impulse[1] = wp.tangent_impulse[1][lane];
            }
        }
    }
}

#if RIME_PHYSICS_WIDE_SSE2
// The velocity iterations over level-ordered batches: solve_velocities, four constraints at a
// time. Read it side by side with solve_velocities in solver.hpp — it is meant to be the same code
// with every float widened to four lanes, and every expression below keeps the scalar
// expression's operand order (that order IS the rounding, so it is load-bearing, not style).
inline void solve_velocities_wide(const SolverBodies& bodies,
                                  std::span<WideContactBatch> batches,
                                  int iterations) noexcept {
    using wide::F4;
    using wide::M4;

    struct V3 {
        F4 x, y, z;
    };

    // core::cross, core::dot and the SymMat3 product, lane-wise, in their exact operand order.
    const auto cross = [](const V3& a, const V3& b) {
        return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const auto dot = [](const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto add = [](const V3& a, const V3& b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; };
    const auto sub = [](const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; };
    const auto scale = [](const V3& a, F4 s) { return V3{a.x * s, a.y * s, a.z * s}; };
    const auto select3 = [](M4 m, const V3& a, const V3& b) {
        return V3{wide::select(m, a.x, b.x), wide::select(m, a.y, b.y), wide::select(m, a.z, b.z)};
    };
    const auto load3 = [](const float (&p)[3][kWideLanes]) {
        return V3{wide::load(p[0]), wide::load(p[1]), wide::load(p[2])};
    };

    struct Sym {
        F4 xx, xy, xz, yy, yz, zz;
    };

    const auto load_sym = [](const float (&p)[6][kWideLanes]) {
        return Sym{wide::load(p[0]),
                   wide::load(p[1]),
                   wide::load(p[2]),
                   wide::load(p[3]),
                   wide::load(p[4]),
                   wide::load(p[5])};
    };
    const auto apply_sym = [](const Sym& m, const V3& l) {
        return V3{m.xx * l.x + m.xy * l.y + m.xz * l.z,
                  m.xy * l.x + m.yy * l.y + m.yz * l.z,
                  m.xz * l.x + m.yz * l.y + m.zz * l.z};
    };

    // Body rows in and out. A Vec3 is three packed floats, so the gather transposes four of them
    // into x/y/z registers through an aligned staging block, and the scatter does the reverse.
    struct alignas(16) Rows {
        float va[3][kWideLanes], wa[3][kWideLanes], vb[3][kWideLanes], wb[3][kWideLanes];
    };

    for (int it = 0; it < iterations; ++it) {
        for (WideContactBatch& w : batches) {
            Rows rows;
            for (std::uint32_t lane = 0; lane < kWideLanes; ++lane) {
                const core::Vec3 va = bodies.linear_velocity[w.body_a[lane]];
                const core::Vec3 wa = bodies.angular_velocity[w.body_a[lane]];
                const core::Vec3 vb = bodies.linear_velocity[w.body_b[lane]];
                const core::Vec3 wb = bodies.angular_velocity[w.body_b[lane]];
                const core::Vec3 in[4] = {va, wa, vb, wb};
                float (*out[4])[kWideLanes] = {rows.va, rows.wa, rows.vb, rows.wb};
                for (int r = 0; r < 4; ++r) {
                    out[r][0][lane] = in[r].x;
                    out[r][1][lane] = in[r].y;
                    out[r][2][lane] = in[r].z;
                }
            }
            V3 va = load3(rows.va);
            V3 wa = load3(rows.wa);
            V3 vb = load3(rows.vb);
            V3 wb = load3(rows.wb);
            const M4 dyn_a = wide::load_mask(w.dyn_a);
            const M4 dyn_b = wide::load_mask(w.dyn_b);
            const F4 ima = wide::load(w.inv_mass_a);
            const F4 imb = wide::load(w.inv_mass_b);
            const Sym inverse_inertia_a = load_sym(w.inverse_inertia_a);
            const Sym inverse_inertia_b = load_sym(w.inverse_inertia_b);
            const V3 normal = load3(w.normal);

            const auto rel_velocity = [&](const V3& r_a, const V3& r_b) {
                return sub(add(vb, cross(wb, r_b)), add(va, cross(wa, r_a)));
            };
            // apply_local, with the M7.5 guard as a mask: a lane whose body is immovable — or which
            // has no point k at all — keeps its old row bit for bit (select, not "add zero": adding
            // a signed zero can flip the sign of a zero velocity, and that is a different hash).
            const auto apply = [&](M4 active, const V3& r_a, const V3& r_b, const V3& p) {
                const M4 upd_a = active & dyn_a;
                const M4 upd_b = active & dyn_b;
                va = select3(upd_a, sub(va, scale(p, ima)), va);
                wa = select3(upd_a, sub(wa, apply_sym(inverse_inertia_a, cross(r_a, p))), wa);
                vb = select3(upd_b, add(vb, scale(p, imb)), vb);
                wb = select3(upd_b, add(wb, apply_sym(inverse_inertia_b, cross(r_b, p))), wb);
            };

            // How many point slots any lane uses. Points fill from k = 0 in every lane, so a slot
            // no lane uses is followed only by more of them; skipping those is skipping work that
            // was all-masked anyway, so it cannot change a bit.
            const int point_slots = w.point_count;

            // Normal constraints: accumulate-and-clamp, λₙ ≥ 0.
            for (int k = 0; k < point_slots; ++k) {
                const M4 active = wide::load_mask(w.point_active[k]);
                WideContactPoint& p = w.points[k];
                const V3 r_a = load3(p.r_a);
                const V3 r_b = load3(p.r_b);
                const F4 vn = dot(rel_velocity(r_a, r_b), normal);
                const F4 delta = wide::load(p.neg_normal_mass) * (vn - wide::load(p.velocity_bias));
                const F4 old = wide::load(p.normal_impulse);
                const F4 sum = old + delta;
                // std::max(sum, 0.0f) is `(sum < 0) ? 0 : sum` — so −0 and NaN pass through
                // unchanged, exactly as they do in the scalar sweep. (_mm_max_ps would not: it
                // returns its SECOND operand on a tie, turning −0 into +0.)
                const F4 accumulated =
                    wide::select(wide::less(sum, wide::zero()), wide::zero(), sum);
                const F4 applied = accumulated - old;
                wide::store(p.normal_impulse, wide::select(active, accumulated, old));
                apply(active, r_a, r_b, scale(normal, applied));
            }

            // Friction pyramid, after all normals, bound by this iteration's fresh λₙ.
            const F4 friction = wide::load(w.friction);
            for (int k = 0; k < point_slots; ++k) {
                const M4 active = wide::load_mask(w.point_active[k]);
                WideContactPoint& p = w.points[k];
                const V3 r_a = load3(p.r_a);
                const V3 r_b = load3(p.r_b);
                const F4 max_friction = friction * wide::load(p.normal_impulse);
                const F4 min_friction = -max_friction;
                for (int t = 0; t < 2; ++t) {
                    const V3 tangent = load3(w.tangent[t]);
                    const F4 vt = dot(rel_velocity(r_a, r_b), tangent);
                    const F4 delta = wide::load(p.neg_tangent_mass[t]) * vt;
                    const F4 old = wide::load(p.tangent_impulse[t]);
                    const F4 sum = old + delta;
                    // std::clamp(sum, lo, hi) as libstdc++ spells it: min(max(sum, lo), hi), i.e.
                    // `m = (sum < lo) ? lo : sum; (hi < m) ? hi : m`. Any lo ≤ hi agrees with the
                    // other standard libraries' spelling too; they differ only on NaN bounds.
                    const F4 lifted =
                        wide::select(wide::less(sum, min_friction), min_friction, sum);
                    const F4 accumulated =
                        wide::select(wide::less(max_friction, lifted), max_friction, lifted);
                    const F4 applied = accumulated - old;
                    wide::store(p.tangent_impulse[t], wide::select(active, accumulated, old));
                    apply(active, r_a, r_b, scale(tangent, applied));
                }
            }

            // Scatter. Lanes of one batch never share a DYNAMIC body (that is what a level is), so
            // these stores cannot collide; an immovable body is never stored, as in the scalar
            // sweep.
            const auto store3 = [](float (&p)[3][kWideLanes], const V3& v) {
                wide::store(p[0], v.x);
                wide::store(p[1], v.y);
                wide::store(p[2], v.z);
            };
            store3(rows.va, va);
            store3(rows.wa, wa);
            store3(rows.vb, vb);
            store3(rows.wb, wb);
            for (std::uint32_t lane = 0; lane < kWideLanes; ++lane) {
                if (w.dyn_a[lane] != 0u) {
                    bodies.linear_velocity[w.body_a[lane]] =
                        core::Vec3{rows.va[0][lane], rows.va[1][lane], rows.va[2][lane]};
                    bodies.angular_velocity[w.body_a[lane]] =
                        core::Vec3{rows.wa[0][lane], rows.wa[1][lane], rows.wa[2][lane]};
                }
                if (w.dyn_b[lane] != 0u) {
                    bodies.linear_velocity[w.body_b[lane]] =
                        core::Vec3{rows.vb[0][lane], rows.vb[1][lane], rows.vb[2][lane]};
                    bodies.angular_velocity[w.body_b[lane]] =
                        core::Vec3{rows.wb[0][lane], rows.wb[1][lane], rows.wb[2][lane]};
                }
            }
        }
    }
}
#endif

} // namespace rime::physics

namespace rime::physics {

// Caller-owned scratch for solve_island_velocities, every pointer already offset to this island.
// world.cpp sizes them per step so that concurrent islands never overlap: `body_depth` is indexed
// by dense body (islands own disjoint dynamic bodies), and the rest by the island's constraint
// range — with `level_start` one entry longer per island.
struct WavefrontScratch {
    std::uint32_t* body_depth = nullptr;  // dense-body indexed, shared by every island
    std::uint32_t* level = nullptr;       // constraints.size() entries
    std::uint32_t* level_start = nullptr; // constraints.size() + 1 entries
    std::uint32_t* order = nullptr;       // constraints.size() entries
    WideContactBatch* batches = nullptr;  // constraints.size() entries (the worst case)
};

// One island's velocity iterations: the wide level-ordered sweep where it can pay, the scalar
// sweep otherwise — the SAME bits either way, so the choice is purely about cost. Returns whether
// the wide path ran (the tests use it to prove they exercised the path they claim to compare).
inline bool solve_island_velocities(const SolverBodies& bodies,
                                    std::span<ContactConstraint> constraints,
                                    std::span<const std::uint32_t> island_bodies,
                                    int iterations,
                                    bool allow_wide,
                                    const WavefrontScratch& scratch) noexcept {
#if RIME_PHYSICS_WIDE_SSE2
    if (allow_wide && constraints.size() >= kWideMinConstraints) {
        for (const std::uint32_t b : island_bodies) {
            scratch.body_depth[b] = 0u;
        }
        const std::uint32_t levels = assign_wavefront_levels(
            constraints, bodies.inv_mass, scratch.body_depth, scratch.level);
        // Worth it only if a level averages at least two constraints: below that most lanes are
        // empty and the gather/scatter around every batch costs more than the lanes save. A chain
        // of single contacts (a rope of debris) is exactly that case.
        if (constraints.size() >= 2u * static_cast<std::size_t>(levels)) {
            const std::size_t batch_count = pack_wide_batches(bodies,
                                                              constraints,
                                                              scratch.level,
                                                              levels,
                                                              scratch.level_start,
                                                              scratch.order,
                                                              scratch.batches);
            const std::span<WideContactBatch> batches{scratch.batches, batch_count};
            solve_velocities_wide(bodies, batches, iterations);
            unpack_wide_impulses(batches, constraints);
            return true;
        }
    }
#else
    (void)island_bodies;
    (void)allow_wide;
    (void)scratch;
#endif
    solve_velocities(bodies, constraints, iterations);
    return false;
}

} // namespace rime::physics
