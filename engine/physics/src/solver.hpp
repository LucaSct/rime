// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/contact.hpp"

// The contact solver (M7.4): SEQUENTIAL IMPULSES — projected Gauss–Seidel (PGS) over the
// narrowphase's contact manifolds — plus a separate non-linear Gauss–Seidel (NGS) position pass
// for penetration recovery. The math is derived step by step in docs/math/sequential-impulse.md
// and the systems reasoning lives in docs/design/physics.md ("Solver"); the short version of each
// idea is inlined where the code does it.
//
// The pipeline one PhysicsWorld::step runs (world.cpp):
//   integrate velocities → detect contacts → PREPARE constraints (effective masses, restitution
//   bias) → WARM-START (re-apply last tick's impulses) → fixed velocity iterations
//   (accumulate-and-clamp) → integrate positions → fixed NGS position iterations (velocities
//   untouched) → commit the solved impulses to the manifold cache.
//
// Determinism (ADR-0026): constraints are prepared and iterated in the manifolds' canonical
// broadphase order, iteration counts are fixed (no convergence early-outs), the friction basis is
// a pure function of the contact normal, and no unordered container is iterated anywhere in the
// solve. Same binary + same inputs ⇒ bit-identical impulses and motion.
//
// Island-readiness (M7.5): every routine below is a pure function of {one constraint, the two
// bodies' rows in SolverBodies} — no globals, no cross-constraint state. Islands share no dynamic
// body, so the island pass can partition the constraint list and run these exact loops per island
// (strictly sequentially inside each) and the result stays bit-identical for any thread count.
//
// Private header (under src/), invisible above the PhysicsWorld seam.
namespace rime::physics {

// ---------------------------------------------------------------------- tuning constants -------
// Iteration counts are FIXED (ADR-0026), never convergence-tested: an early-out would make the
// number of float operations depend on intermediate values and break bit-identical replays. 8/2
// is the budget the ADR pins (Box2D ships 8/3, Jolt 10/2): PGS error shrinks geometrically per
// sweep, so eight velocity sweeps hold small stacks, and the position error only needs a nudge
// per tick because NGS re-runs every tick against freshly recomputed geometry.
inline constexpr int kVelocityIterations = 8;
inline constexpr int kPositionIterations = 2;

// NGS deliberately leaves this much penetration in place (metres). Recovering the FULL overlap
// would make a resting contact oscillate between "touching" (manifold exists, gets pushed out)
// and "separated" (no manifold, the body falls back in) on alternating ticks; keeping a few
// millimetres of slop means the manifold — and the warm-start cache keyed off it — persists
// tick after tick while a body rests.
inline constexpr float kPenetrationSlop = 0.005f;

// Fraction of the beyond-slop penetration one NGS iteration corrects, plus a hard cap per point
// per iteration. Correcting 100% at once overshoots: each point's correction disturbs its
// neighbours' (that is the nature of Gauss–Seidel), so full steps ping-pong instead of settling;
// 20% per iteration damps the coupling. The cap bounds how far a pathological overlap (a body
// spawned inside a wall) can teleport in a single tick.
inline constexpr float kNgsCorrectionRate = 0.2f;
inline constexpr float kNgsMaxCorrection = 0.2f; // metres

// Restitution only applies above this approach speed (m/s). A resting body re-approaches its
// support at ~g·dt every tick (gravity integrates before the solve), and bouncing THAT off would
// make resting contact buzz forever; below the threshold the solver's target velocity is simply
// zero. 1 m/s is the classic gate (Box2D uses the same number).
inline constexpr float kRestitutionThreshold = 1.0f;

// -------------------------------------------------------------------------- body access --------
// The solver's non-owning view of PhysicsWorld's SoA body pool (world.cpp builds one per step).
// Raw pointers, dense-indexed: the solver reads and writes exactly the arrays the integrator
// does, with no copies — data-oriented, and trivially partitionable for the M7.5 island pass.
struct SolverBodies {
    core::Vec3* position = nullptr;
    core::Quat* orientation = nullptr;
    core::Vec3* linear_velocity = nullptr;
    core::Vec3* angular_velocity = nullptr;
    const float* inv_mass = nullptr;         // 0 ⇒ immovable (static/kinematic)
    const core::Vec3* inv_inertia = nullptr; // body-space diagonal (shape principal axes)
    const float* friction = nullptr;         // Coulomb μ per body
    const float* restitution = nullptr;      // bounciness e per body
};

// I_world⁻¹·L without ever building a 3×3 matrix. The stored inverse inertia is the body-space
// diagonal (our primitives' principal axes), and I_world⁻¹ = R·diag(i)·Rᵀ — so rotate L into body
// space, scale per axis, rotate back. Two quaternion rotates keep us in the exact math vocabulary
// the integrator already uses (a unit quaternion's inverse is its conjugate).
[[nodiscard]] inline core::Vec3
apply_inv_inertia(const core::Quat& q, core::Vec3 inv_inertia_body, core::Vec3 l) noexcept {
    const core::Vec3 body = core::rotate(core::conjugate(q), l);
    return core::rotate(q,
                        core::Vec3{body.x * inv_inertia_body.x,
                                   body.y * inv_inertia_body.y,
                                   body.z * inv_inertia_body.z});
}

// A deterministic orthonormal tangent basis for a contact normal — the two friction directions.
// Built from the world axis LEAST aligned with n (fixed X→Y→Z scan with strict <, so ties resolve
// identically every call): cross(n, that axis) is then farthest from degenerate. The basis is a
// pure function of n alone, so a persistent contact re-derives the SAME basis every tick and the
// cached tangent impulse keeps its meaning across frames (warm starting).
inline void make_tangent_basis(core::Vec3 n, core::Vec3& t1, core::Vec3& t2) noexcept {
    core::Vec3 e{1.0f, 0.0f, 0.0f};
    float best = std::fabs(n.x);
    if (std::fabs(n.y) < best) {
        best = std::fabs(n.y);
        e = core::Vec3{0.0f, 1.0f, 0.0f};
    }
    if (std::fabs(n.z) < best) {
        e = core::Vec3{0.0f, 0.0f, 1.0f};
    }
    t1 = core::normalize(core::cross(n, e));
    t2 = core::cross(n, t1); // already unit: n ⟂ t1 and both are unit
}

// First-order orientation nudge by an angular DISPLACEMENT dtheta (axis·angle, radians): the
// positional twin of the integrator's q̇ = ½ω⊗q with the finite angle in place of ω·dt
// (docs/math/rigid-body-dynamics.md §3). The NGS pass uses it to rotate bodies out of
// penetration without ever touching their angular velocity.
inline void rotate_orientation(core::Quat& q, core::Vec3 dtheta) noexcept {
    const core::Quat w{dtheta.x, dtheta.y, dtheta.z, 0.0f};
    q = core::normalize(q + (w * q) * 0.5f);
}

// ------------------------------------------------------------------ prepared constraints -------
// Everything one contact point needs during iteration, precomputed once per tick so the hot
// PGS loop is a handful of dots and multiplies per point.
struct ContactPointConstraint {
    core::Vec3 r_a;            // COM(a) → contact point, world (the velocity-phase lever arm)
    core::Vec3 r_b;            // COM(b) → contact point, world
    core::Vec3 local_anchor_a; // a's surface point in a's body frame (NGS re-poses it)
    core::Vec3 local_anchor_b; // b's surface point in b's body frame
    float normal_mass = 0.0f;  // 1 / (J M⁻¹ Jᵀ) along the normal
    float tangent_mass[2] = {0.0f, 0.0f};    // same, along each friction direction
    float velocity_bias = 0.0f;              // restitution target: −e·(approach speed), else 0
    float normal_impulse = 0.0f;             // accumulated λₙ (starts at the warm-start value)
    float tangent_impulse[2] = {0.0f, 0.0f}; // accumulated friction λ (t2 has no cached slot)
};

struct ContactConstraint {
    std::uint32_t body_a = 0; // dense indices into SolverBodies (canonical: a < b's slot)
    std::uint32_t body_b = 0;
    std::uint32_t manifold_index = 0; // where store_impulses writes the converged λ back
    core::Vec3 normal;                // unit, points a → b (contact.hpp convention)
    core::Vec3 tangent[2];            // friction directions (make_tangent_basis)
    core::Vec3 local_normal_a;        // normal in a's frame: NGS re-derives n as a rotates
    float friction = 0.0f;            // combined μ for the pair
    std::uint8_t count = 0;
    ContactPointConstraint points[4];
};

// Apply one impulse `p` at a contact point: +p to body b at r_b, −p to body a at r_a. This ±
// pair IS Newton's third law — total linear and angular momentum of the pair are conserved by
// construction, which the momentum test observes directly.
inline void apply_impulse(const SolverBodies& bodies,
                          std::uint32_t ia,
                          std::uint32_t ib,
                          core::Vec3 r_a,
                          core::Vec3 r_b,
                          core::Vec3 p) noexcept {
    bodies.linear_velocity[ia] -= p * bodies.inv_mass[ia];
    bodies.angular_velocity[ia] -=
        apply_inv_inertia(bodies.orientation[ia], bodies.inv_inertia[ia], core::cross(r_a, p));
    bodies.linear_velocity[ib] += p * bodies.inv_mass[ib];
    bodies.angular_velocity[ib] +=
        apply_inv_inertia(bodies.orientation[ib], bodies.inv_inertia[ib], core::cross(r_b, p));
}

// Build the prepared constraint for one manifold. `body_a`/`body_b` are the manifold bodies'
// dense indices — the caller resolves them and guarantees at least one is dynamic (a pair with
// no dynamic member has nothing to solve).
//
// Material combine rules (each engine picks its own; ours, documented):
//   μ = √(μ_a·μ_b) — the geometric mean: zero if either surface is frictionless, symmetric, and
//     it never exceeds the rougher surface (an arithmetic mean would let ice-on-rubber keep half
//     of rubber's grip).
//   e = max(e_a, e_b) — the bouncier material wins: a rubber ball bounces on concrete even
//     though concrete on concrete does not.
[[nodiscard]] inline ContactConstraint
prepare_contact_constraint(const SolverBodies& bodies,
                           const Manifold& m,
                           std::uint32_t body_a,
                           std::uint32_t body_b,
                           std::uint32_t manifold_index) noexcept {
    const core::Vec3 xa = bodies.position[body_a];
    const core::Vec3 xb = bodies.position[body_b];
    const core::Quat qa = bodies.orientation[body_a];
    const core::Quat qb = bodies.orientation[body_b];
    const core::Vec3 va = bodies.linear_velocity[body_a];
    const core::Vec3 vb = bodies.linear_velocity[body_b];
    const core::Vec3 wa = bodies.angular_velocity[body_a];
    const core::Vec3 wb = bodies.angular_velocity[body_b];
    const float ima = bodies.inv_mass[body_a];
    const float imb = bodies.inv_mass[body_b];
    const core::Vec3 iia = bodies.inv_inertia[body_a];
    const core::Vec3 iib = bodies.inv_inertia[body_b];

    ContactConstraint c;
    c.body_a = body_a;
    c.body_b = body_b;
    c.manifold_index = manifold_index;
    c.normal = m.normal;
    make_tangent_basis(m.normal, c.tangent[0], c.tangent[1]);
    c.local_normal_a = core::rotate(core::conjugate(qa), m.normal);
    c.friction = std::sqrt(bodies.friction[body_a] * bodies.friction[body_b]);
    const float restitution = std::max(bodies.restitution[body_a], bodies.restitution[body_b]);
    c.count = m.count;

    for (std::uint8_t k = 0; k < m.count; ++k) {
        const ContactPoint& p = m.points[k];
        ContactPointConstraint& out = c.points[k];
        out.r_a = p.position - xa; // positions ARE the centres of mass for the v1 primitives
        out.r_b = p.position - xb;

        // EFFECTIVE MASS along a direction d (sequential-impulse.md §2):
        //   k_d = 1/m_a + 1/m_b + ((I_a⁻¹(r_a×d))×r_a + (I_b⁻¹(r_b×d))×r_b)·d
        // — how much relative velocity along d one unit of impulse buys, folding in how much of
        // the impulse leaks into rotation at these lever arms. Its inverse turns "close this
        // velocity error" into an impulse with a single multiply.
        const auto effective_mass = [&](core::Vec3 d) {
            const core::Vec3 ang_a =
                core::cross(apply_inv_inertia(qa, iia, core::cross(out.r_a, d)), out.r_a);
            const core::Vec3 ang_b =
                core::cross(apply_inv_inertia(qb, iib, core::cross(out.r_b, d)), out.r_b);
            const float k_d = ima + imb + core::dot(ang_a + ang_b, d);
            return k_d > 0.0f ? 1.0f / k_d : 0.0f;
        };
        out.normal_mass = effective_mass(c.normal);
        out.tangent_mass[0] = effective_mass(c.tangent[0]);
        out.tangent_mass[1] = effective_mass(c.tangent[1]);

        // Restitution bias, from the PRE-solve approach speed (gravity already integrated, warm
        // start not yet applied — every constraint's bias is measured against the same state).
        // vₙ < 0 means approaching; the gate keeps resting contacts (approach ≈ g·dt) dead.
        const core::Vec3 v_rel = (vb + core::cross(wb, out.r_b)) - (va + core::cross(wa, out.r_a));
        const float vn = core::dot(v_rel, c.normal);
        out.velocity_bias = vn < -kRestitutionThreshold ? -restitution * vn : 0.0f;

        // NGS anchors: the manifold stores the MIDPOINT of the two surfaces, so the surfaces
        // themselves sit ±penetration/2 along the normal. Each surface point is stored in its own
        // body's local frame so the position pass can re-pose it as the bodies move and measure
        // the REAL current separation instead of trusting a stale depth.
        const core::Vec3 surf_a = p.position + c.normal * (p.penetration * 0.5f);
        const core::Vec3 surf_b = p.position - c.normal * (p.penetration * 0.5f);
        out.local_anchor_a = core::rotate(core::conjugate(qa), surf_a - xa);
        out.local_anchor_b = core::rotate(core::conjugate(qb), surf_b - xb);

        // The accumulators START at the cache-carried impulses (zero for a brand-new contact);
        // warm_start() applies them to the bodies. The second tangent has no cached slot
        // (ContactPoint persists one tangent scalar), so it re-converges from zero each tick.
        out.normal_impulse = p.normal_impulse;
        out.tangent_impulse[0] = p.tangent_impulse;
        out.tangent_impulse[1] = 0.0f;
    }
    return c;
}

// WARM STARTING: re-apply last tick's converged impulses before iterating. PGS converges
// geometrically from wherever it starts; a resting stack needs the same large support impulses
// every tick, and eight iterations from zero never quite reach them — the stack sags and buzzes.
// Started from last tick's solution, iteration only has to correct the (tiny) change since then.
// Accumulate-and-clamp keeps this honest: what a tick applies in total is exactly the final
// accumulator value, so if the cached impulse is now too big (the box just left the ground) the
// iterations subtract the excess right back out.
inline void warm_start(const SolverBodies& bodies,
                       std::span<ContactConstraint> constraints) noexcept {
    for (ContactConstraint& c : constraints) {
        for (std::uint8_t k = 0; k < c.count; ++k) {
            const ContactPointConstraint& p = c.points[k];
            const core::Vec3 impulse = c.normal * p.normal_impulse +
                                       c.tangent[0] * p.tangent_impulse[0] +
                                       c.tangent[1] * p.tangent_impulse[1];
            apply_impulse(bodies, c.body_a, c.body_b, p.r_a, p.r_b, impulse);
        }
    }
}

// The velocity solve: `iterations` sequential-impulse sweeps over every contact point, in
// constraint order (Gauss–Seidel — each solve sees every earlier solve's velocity updates, which
// is exactly what propagates support forces up through a stack).
inline void solve_velocities(const SolverBodies& bodies,
                             std::span<ContactConstraint> constraints,
                             int iterations) noexcept {
    for (int it = 0; it < iterations; ++it) {
        for (ContactConstraint& c : constraints) {
            const std::uint32_t ia = c.body_a;
            const std::uint32_t ib = c.body_b;

            // Relative velocity of the contact point on b w.r.t. the point on a: a POSITIVE
            // normal component means separating (the normal points a → b).
            const auto rel_velocity = [&](const ContactPointConstraint& p) {
                return (bodies.linear_velocity[ib] +
                        core::cross(bodies.angular_velocity[ib], p.r_b)) -
                       (bodies.linear_velocity[ia] +
                        core::cross(bodies.angular_velocity[ia], p.r_a));
            };

            // Normal constraint: drive vₙ to the bias (0, or the restitution target).
            // ACCUMULATE-AND-CLAMP (Catto): clamp the running TOTAL λₙ ≥ 0, never the
            // per-iteration delta. Deltas may legitimately be negative mid-solve (taking back an
            // over-push from an earlier iteration or the warm start); only the total must stay a
            // push. Clamping deltas instead would freeze those corrections in — the classic
            // sticky-contact bug this formulation exists to fix.
            for (std::uint8_t k = 0; k < c.count; ++k) {
                ContactPointConstraint& p = c.points[k];
                const float vn = core::dot(rel_velocity(p), c.normal);
                const float delta = -p.normal_mass * (vn - p.velocity_bias);
                const float accumulated = std::max(p.normal_impulse + delta, 0.0f);
                const float applied = accumulated - p.normal_impulse;
                p.normal_impulse = accumulated;
                apply_impulse(bodies, ia, ib, p.r_a, p.r_b, c.normal * applied);
            }

            // FRICTION PYRAMID, solved after the normals so the bound |λ_t| ≤ μ·λₙ uses this
            // iteration's fresh normal impulse. Two independent clamped axes approximate the
            // Coulomb cone by its inscribed pyramid: sliding diagonally can recruit up to √2·μ,
            // the standard linearization error, accepted for never having to solve a coupled
            // 2-D projection in the hot loop.
            for (std::uint8_t k = 0; k < c.count; ++k) {
                ContactPointConstraint& p = c.points[k];
                const float max_friction = c.friction * p.normal_impulse;
                for (int t = 0; t < 2; ++t) {
                    const float vt = core::dot(rel_velocity(p), c.tangent[t]);
                    const float delta = -p.tangent_mass[t] * vt;
                    const float accumulated =
                        std::clamp(p.tangent_impulse[t] + delta, -max_friction, max_friction);
                    const float applied = accumulated - p.tangent_impulse[t];
                    p.tangent_impulse[t] = accumulated;
                    apply_impulse(bodies, ia, ib, p.r_a, p.r_b, c.tangent[t] * applied);
                }
            }
        }
    }
}

// Persist the converged accumulators back into the manifold points; world.cpp then commits those
// manifolds to the contact cache, closing the warm-start loop (build → solve → commit). Only the
// primary tangent has a cached slot — the secondary re-converges from zero each tick, cheap
// because the primary carries the dominant sliding direction.
inline void store_impulses(std::span<const ContactConstraint> constraints,
                           std::span<Manifold> manifolds) noexcept {
    for (const ContactConstraint& c : constraints) {
        Manifold& m = manifolds[c.manifold_index];
        for (std::uint8_t k = 0; k < c.count; ++k) {
            m.points[k].normal_impulse = c.points[k].normal_impulse;
            m.points[k].tangent_impulse = c.points[k].tangent_impulse[0];
        }
    }
}

// The NGS POSITION PASS — penetration recovery that never touches velocity, and the reason this
// solver is NOT Baumgarte stabilization (ADR-0026). Baumgarte adds β/dt·penetration to the
// velocity constraint's bias: one solver, but the correction velocity is REAL — bodies exit deep
// contacts with kinetic energy they never earned (debris that pops upward, stacks that buzz and
// never sleep). NGS instead runs a second Gauss–Seidel pass directly on positions and
// orientations after integration: pseudo-impulses displace poses, and the velocity arrays are
// never read or written, so no kinetic energy can enter (the split-impulse family; the tests pin
// exactly this property — deep overlap resolves with speeds staying at zero).
//
// "Non-linear": each iteration re-poses the contact anchors with the bodies' CURRENT
// positions/orientations and re-measures the true separation, rather than solving a linearized
// system frozen at tick start — corrections chase the geometry as it moves, which is what lets a
// fixed two iterations converge tick over tick without overshoot.
inline void solve_positions(const SolverBodies& bodies,
                            std::span<ContactConstraint> constraints,
                            int iterations) noexcept {
    for (int it = 0; it < iterations; ++it) {
        for (const ContactConstraint& c : constraints) {
            const std::uint32_t ia = c.body_a;
            const std::uint32_t ib = c.body_b;
            const float ima = bodies.inv_mass[ia];
            const float imb = bodies.inv_mass[ib];
            for (std::uint8_t k = 0; k < c.count; ++k) {
                const ContactPointConstraint& p = c.points[k];
                const core::Vec3 xa = bodies.position[ia];
                const core::Vec3 xb = bodies.position[ib];
                const core::Quat qa = bodies.orientation[ia];
                const core::Quat qb = bodies.orientation[ib];

                // Re-pose: the anchors ride their bodies; the normal rides body a. (v1 pins the
                // normal to a's frame whichever body owned the reference face — at worst the push
                // direction is one tick stale, which the slop, the rate, and next tick's fresh
                // manifold absorb.)
                const core::Vec3 n = core::rotate(qa, c.local_normal_a);
                const core::Vec3 pa = xa + core::rotate(qa, p.local_anchor_a);
                const core::Vec3 pb = xb + core::rotate(qb, p.local_anchor_b);
                const float separation = core::dot(pb - pa, n); // < 0 ⇒ still penetrating

                // Correct a fraction of the beyond-slop penetration, hard-capped per iteration.
                const float correction = std::clamp(
                    kNgsCorrectionRate * (separation + kPenetrationSlop), -kNgsMaxCorrection, 0.0f);
                if (!(correction < 0.0f)) {
                    continue; // separated, or within the slop: leave it be
                }

                // Effective mass at the CURRENT poses (the anchors moved), applied at the middle
                // of the two surface points — the least-biased application point, mirroring the
                // manifold's midpoint convention.
                const core::Vec3 mid = (pa + pb) * 0.5f;
                const core::Vec3 r_a = mid - xa;
                const core::Vec3 r_b = mid - xb;
                const core::Vec3 ang_a = core::cross(
                    apply_inv_inertia(qa, bodies.inv_inertia[ia], core::cross(r_a, n)), r_a);
                const core::Vec3 ang_b = core::cross(
                    apply_inv_inertia(qb, bodies.inv_inertia[ib], core::cross(r_b, n)), r_b);
                const float k_n = ima + imb + core::dot(ang_a + ang_b, n);
                if (k_n <= 0.0f) {
                    continue;
                }

                // A pseudo-impulse in displacement units: mass-weighted exactly like a real
                // impulse (the heavier body yields less ground), but applied to POSE only.
                const core::Vec3 impulse = n * (-correction / k_n);
                bodies.position[ia] -= impulse * ima;
                rotate_orientation(
                    bodies.orientation[ia],
                    -apply_inv_inertia(qa, bodies.inv_inertia[ia], core::cross(r_a, impulse)));
                bodies.position[ib] += impulse * imb;
                rotate_orientation(
                    bodies.orientation[ib],
                    apply_inv_inertia(qb, bodies.inv_inertia[ib], core::cross(r_b, impulse)));
            }
        }
    }
}

} // namespace rime::physics
