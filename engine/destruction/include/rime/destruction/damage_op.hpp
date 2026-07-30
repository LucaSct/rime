// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/vec.hpp"
#include "rime/destruction/ids.hpp"

// DamageOp — the normalized unit of destruction damage, promoted to the public seam by m11.4.
//
// WHY THIS TYPE IS PUBLIC NOW. Through M8 this was an implementation detail of damage.cpp: the
// currency both damage sources (explicit `apply_damage` calls and solver contact impulses) reduce
// to before anything is applied. ADR-0033 amendment A1 made it a wire contract. The reasoning is
// worth restating here, because it is the whole reason networked destruction works at all:
//
//   Replaying *commands* does not reproduce destruction. Half the damage stream is emergent rather
//   than commanded — the runtime converts the local solver's contact impulses into damage ops every
//   tick. A client that replayed only explicit `apply_damage` calls would diverge from the server
//   the first time a debris pile eroded a part by resting on it. So the replicated artifact is the
//   COMMITTED, CANONICALLY-ORDERED OP LIST — which was always the deterministic function's input
//   (ADR-0029 §3), and is now also what crosses the wire.
//
// A PROPERTY WORTH NAMING: THE FALLOFF ARITHMETIC RUNS EXACTLY ONCE. These are *expanded* per-part
// ops, not the `apply_damage` calls that produced them. A radius call fans out to one op per
// overlapped part with the linear falloff already resolved into `amount` and `impulse`. Replicating
// the expansion rather than the call means that float arithmetic happens on the server and nowhere
// else — two peers cannot disagree about it, because only one of them ever computed it. Replicating
// the call would have re-run `distance_to_part_bounds` and the falloff multiply on every client, on
// a different compiler, and invited exactly the sub-ULP drift the convergence proof exists to
// catch.
namespace rime::destruction {

// One damage operation against one part. `impulse` is the world-space push the op carries into
// whatever debris body its part detaches with; `central` selects how it is applied — contact ops
// push at their contact `point` (the real surface point, so the lever arm is part of the look),
// explicit ops push through the COM, because a blast centre is usually outside the body and an
// invented lever arm there would add spin the caller never asked for.
struct DamageOp {
    InstanceId instance{};
    std::uint32_t part = 0;
    float amount = 0.0f;
    core::Vec3 impulse{0.0f, 0.0f, 0.0f};
    core::Vec3 point{0.0f, 0.0f, 0.0f};
    bool central = false;
};

} // namespace rime::destruction
