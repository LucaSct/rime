// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/vec.hpp"
#include "rime/physics/body.hpp"

// Contact manifolds — the narrowphase's output (M7.3) and the solver's input (M7.4). A manifold
// describes where two bodies touch: a shared contact normal plus up to four contact points. Four
// is enough to represent any stable planar contact patch (a box resting on the ground needs
// exactly four to not tip), and a fixed-size array keeps manifolds POD and pool-friendly — no
// allocation on the contact path, which at destruction scale runs thousands of times per tick.
//
// Conventions (every producer and consumer of these types relies on them, so they are spelled out
// precisely here and asserted in tests/physics/narrowphase_test.cpp):
//
//  - `Manifold::normal` is unit length and points FROM body `a` TOWARD body `b`: it is the
//    direction to push `b` (and the opposite of the direction to push `a`) to separate the pair.
//    `a`/`b` follow the broadphase's canonical order (a's slot id < b's slot id), so the sign is
//    reproducible run to run.
//  - `ContactPoint::penetration` is the overlap depth along `normal` at that point: the two
//    surfaces would separate after translating `b` by `normal * penetration`. It is >= 0 for a real
//    (overlapping) contact — the exact narrowphase emits nothing for a separated pair. A
//    SPECULATIVE CCD contact (M7.10) is the one exception: it carries a NEGATIVE penetration — the
//    size of the gap the two surfaces have yet to close — so the solver can arrest a fast body
//    exactly at the surface instead of letting it tunnel through in one step (see the solver's
//    speculative bias).
//  - `ContactPoint::position` lies on the nominal contact surface: midway between the two bodies'
//    surfaces along `normal` (the two coincide as penetration -> 0). The midpoint is the
//    least-biased anchor for a solver that applies equal-and-opposite impulses at the point.
//  - `ContactPoint::feature_id` is a stable hash of the shape *features* (which face/edge/corner
//    of each body) that produced the point. The same geometric contact therefore keeps the same
//    id frame after frame, while distinct points within one manifold get distinct ids. That
//    stability is what makes warm-starting possible: the persistent manifold cache matches this
//    frame's points to last frame's BY feature id and carries their accumulated impulses forward,
//    so the M7.4 solver starts each tick from last tick's converged solution instead of from zero.
//  - `normal_impulse` / `tangent_impulse` are that warm-start storage. M7.3 zeroes them (there is
//    no solver yet); from M7.4 they hold the impulses accumulated by the last solve, matched
//    across frames by feature id.
//  - Since M7.12 (compound shapes, ADR-0028) a body pair may carry SEVERAL manifolds — one per
//    touching pair of compound children (a dumbbell-shaped compound standing on the floor touches
//    in two places; no single 4-point patch can hold both feet down). `child_a`/`child_b` name the
//    contact region: the index of the compound child on each side, 0 for a non-compound body. The
//    manifold list stays canonically ordered — by pair as before, then by (child_a, child_b)
//    within a pair — and a non-compound world still gets exactly one manifold per pair, unchanged.
namespace rime::physics {

struct ContactPoint {
    core::Vec3 position{0.0f, 0.0f, 0.0f}; // midway between the two surfaces (see above)
    float penetration = 0.0f;              // overlap depth along the normal; >=0, or <0 for a
                                           // speculative CCD gap (M7.10; see the convention note)
    std::uint32_t feature_id = 0;          // stable id of the generating shape features
    float normal_impulse = 0.0f;           // accumulated impulse along `normal` (M7.4 warm start)
    float tangent_impulse = 0.0f;          // accumulated friction impulse (M7.4 warm start)
};

struct Manifold {
    BodyId a; // canonical: a.index < b.index (broadphase order)
    BodyId b;
    core::Vec3 normal{0.0f, 1.0f, 0.0f}; // unit, points from `a` toward `b`
    ContactPoint points[4];              // only the first `count` entries are meaningful
    std::uint8_t count = 0;    // 0 never leaves the narrowphase (no-contact = no manifold)
    std::uint16_t child_a = 0; // compound child index on each side (M7.12) — which part of a
    std::uint16_t child_b = 0; // compound this contact region belongs to; 0 for a plain body
};

} // namespace rime::physics
