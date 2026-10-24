// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>

#include "rime/core/math/vec.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/query.hpp"

namespace rime::block_play {

// RE-ANCHORING A BULLET SCAR AFTER ITS BODY DIES.
//
// A scar is stored in the local frame of the body it was shot into, so a moving target carries
// it. Fracture replaces that body: the old BodyId goes stale and the pieces that result are NEW
// bodies. Dropping the scar then erases every mark on the wall at once, including those on
// pieces that never moved. The cure is to ask the physics world "what is solid at the place this
// scar last was?" -- a short ray from just outside the surface, back into it. The new body that
// answers either sits where the scar was (the piece stayed: re-anchor, the world pose is
// unchanged) or it does not (the piece left: drop). Pure over a ray callback so the rule can
// be tested without a GPU or a world.
inline constexpr float kScarProbeBackoff = 0.05f;   // start the ray this far off the surface
inline constexpr float kScarProbeReach = 0.10f;     // total ray length, along -normal
inline constexpr float kScarProbeTolerance = 0.02f; // the hit must be this near the old point

struct ScarProbeHit {
    physics::BodyId body{};
    core::Vec3 point{};
};

// `cast(origin, direction, max_distance, ScarProbeHit&) -> bool`. `normal` must be unit length.
// On success `body`/`point` receive the new anchor body and the surface point that justified it.
template <class Cast>
[[nodiscard]] bool
find_scar_anchor(core::Vec3 point, core::Vec3 normal, Cast&& cast, ScarProbeHit& out) {
    const core::Vec3 origin = point + normal * kScarProbeBackoff;
    ScarProbeHit hit{};
    if (!cast(origin, -normal, kScarProbeReach, hit))
        return false;
    if (core::length(hit.point - point) > kScarProbeTolerance)
        return false;
    out = hit;
    return true;
}

} // namespace rime::block_play
