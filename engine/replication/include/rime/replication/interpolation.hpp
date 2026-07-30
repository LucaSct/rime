// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

#include "rime/core/math/transform.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/world.hpp"

// Client-side transform history (m11.6) — the previous/current pair that lets a renderer draw
// between two ticks instead of snapping to the newest one.
//
// This is the buffer ADR-0023 §3 left as a documented seam. The alpha has been computed and handed
// to the render callback since M2; what was missing is the *previous* state to blend it against.
//
// WHY A COMPONENT AND NOT A SIDE TABLE. The obvious implementation is an array indexed by
// `NetId::index`, parallel to the ones ServerReplicator already keeps. It would be wrong for a
// reason this module has now paid for five times: a slot-keyed record outlives the entity that
// owned the slot, and the next tenant inherits it. Here that would mean a freshly spawned entity
// blending out of a dead entity's last position — a visible smear across the level, from a bug with
// no wrong state anywhere. As a component it is destroyed with its entity by the ECS's own
// generational safety, and the whole class of mistake is unrepresentable.
//
// WHY IT IS NOT REFLECTED. It is local presentation state, derived on the receiver, never a claim
// about what any peer holds — the same category `docs/design/replication.md` puts the interpolation
// alpha in. Staying unreflected keeps it out of the wire schema and out of the component schema
// hash, so a client may carry it while the server does not and the handshake still matches.
namespace rime::replication {

// The transform this mirror held BEFORE the most recent replicated write, and whether there is one.
//
// `valid` is not a nicety. A newly appeared entity — a fresh spawn, or one entering relevancy range
// — has no previous, and blending against a default-constructed Transform would launch it from the
// world origin every time. It must SNAP. Structurally the same case as the relevancy-entry send
// (an entity the receiver has no prior state for), so it is solved the same way: an explicit "do I
// have one" flag rather than a sentinel value that some other code path might produce by accident.
//
// `valid` also has to be turned back OFF, which is less obvious and was the defect m11.6b found.
// It means "a genuinely new value landed during the tick that just ran", and therefore has a
// lifetime of exactly one tick: `alpha` sweeps 0→1 every tick period whether or not this entity
// received anything, so a pair left valid after the motion stopped replays that last step forever —
// the mirror visibly snaps back and slides forward, once per tick, for as long as it stands still.
// ClientReplicator::settle_transform_history() is what expires it; `moved_this_tick` is how that
// pass tells "still moving" from "finished".
struct PreviousTransform {
    core::Transform value{};
    bool valid = false;
    // Set by the apply path when a genuinely different value lands; cleared once per tick by
    // ClientReplicator::settle_transform_history(). Bookkeeping, not state a reader should consult.
    bool moved_this_tick = false;
};

// Where to draw `entity` at `alpha` in [0, 1) between the previous tick and the current one.
//
// Returns the current transform unchanged when there is no valid previous — the snap case above.
// `alpha` is clamped, so a caller that hands over a stale accumulator gets a sane frame rather than
// extrapolation it did not ask for.
[[nodiscard]] core::Transform
interpolated_transform(const ecs::World& world, ecs::Entity entity, float alpha) noexcept;

// Deposit this frame's blended pose into `ecs::RenderTransform` for every mirror carrying transform
// history — the consumer half of m11.6, and the only thing that makes the blend visible.
//
// CALL IT ONCE PER FRAME, FROM THE RENDER CALLBACK — never as a sim stage. Two reasons, and the
// first is the one that matters: `alpha` is wall-clock-derived, so a tick that could see this
// result would no longer be deterministic, and M11's whole cross-peer proof rests on ticks being
// deterministic. Application hands `alpha` out through exactly one channel (FrameContext), which is
// why this function can only be reached from the one place it is safe to reach it from. The second
// reason is simply that a per-tick call would compute a pose for a frame that never happened.
//
// Ordering within the frame: after the frame's ticks have run (so the pose being blended TO is the
// newest one applied) and before the scene is extracted. Composing against a parent uses that
// parent's simulated `WorldTransform`, so it also wants propagate_transforms to have run — which
// the tick already guarantees.
//
// Cost is proportional to mirrors carrying history, never to world size, and a mirror that has
// settled costs a copy rather than a blend (interpolated_transform early-outs on it). Returns how
// many entities were written — the counter a proof asserts on, since a pass that silently visited
// nothing reads exactly like a pass that worked.
std::size_t update_render_transforms(ecs::World& world, float alpha);

} // namespace rime::replication
