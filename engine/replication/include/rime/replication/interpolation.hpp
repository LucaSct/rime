// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>

#include "rime/core/math/transform.hpp"
#include "rime/ecs/chunk.hpp" // ecs::Version
#include "rime/ecs/entity.hpp"
#include "rime/ecs/world.hpp"

// Client-side transform history (m11.6, v2 at m12.5) — the previous/current pair that lets a
// renderer draw between two ticks instead of snapping to the newest one.
//
// This is the buffer ADR-0023 §3 left as a documented seam. The alpha has been computed and handed
// to the render callback since M2; what was missing is the *previous* state to blend it against.
//
// ── WHAT v1 GOT WRONG, AND WHY IT ONLY SHOWS AT SCALE (m12.5) ────────────────────────────────
//
// v1 blended previous→current over EXACTLY ONE local tick: `alpha` sweeps 0→1 once per tick period,
// and `settle_transform_history` then expired the pair. That is right if and only if a value
// arrives every tick — which is the one case m11.6's proof exercised, and not the case a real
// session is in. Loss drops snapshots; relevancy holds distant entities back; the byte budget
// defers records; and the server sends nothing at all for an entity that did not change. So a
// mirror routinely receives a value covering N server ticks of motion.
//
// v1 then played those N ticks of motion in ONE tick and held still for the other N−1. The mirror
// lurches and freezes, lurches and freezes — at a rate set by how badly the link is behaving, which
// is exactly when a player is least forgiving. Nothing about it is detectable from state: the
// positions are all correct, and every convergence proof stays green.
//
// v2 blends over THE INTERVAL THE VALUE ACTUALLY COVERS. The Delta header already carries the
// server tick (snapshot.hpp), so the gap between two values for the same mirror is known exactly,
// with no clock synchronisation anywhere: it is a difference of two server ticks, and a difference
// needs no shared origin. `span_ticks` is that gap; the blend runs across that many local tick
// periods instead of one.
//
// A value arriving every tick gives span 1 and v2 is then bit-identical to v1 — which is what lets
// m11.6's proofs stand unchanged, and is the first thing m12.5's own proof asserts.
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

    // ── v2 (m12.5): how long this blend is, and how far through it we are ─────────────────────

    // Local tick periods the previous→current blend covers, derived from the difference between
    // the server ticks of the two values. Never 0 (that would divide by nothing) and never above
    // `kMaxInterpolationSpan` — see there for what happens instead.
    std::uint16_t span_ticks = 1;

    // Whole tick periods of this blend already shown. Advanced once per tick by
    // settle_transform_history, and deliberately NOT advanced on the tick the value arrived: the
    // frames that follow that tick are the blend's FIRST period, so they must sample at 0.
    std::uint16_t elapsed_ticks = 0;

    // The server tick `current` was written at — the other half of the subtraction that produces
    // `span_ticks`. It is a bare difference of two server ticks, so it says nothing about what time
    // it is on either machine and needs no clock offset (ADR-0033 A11 rules one out; this does not
    // want one).
    ecs::Version source_tick = 0;
};

// The longest blend v2 will stretch to. Past it the mirror SNAPS instead, and the snap is counted
// (`ClientReplicator::histories_snapped_far`).
//
// It is a bound on presentation lag, and it is what makes "smooth" not turn into "wrong". A mirror
// that was culled by relevancy for two seconds and then re-entered carries a gap of 120 ticks; a
// blend stretched over that would crawl the entity across the level in slow motion while the
// authority already has it somewhere else — visibly worse than the snap it replaced. Eight ticks is
// ~133 ms at 60 Hz: long enough to cover ordinary loss and jitter, short enough that the eye reads
// the result as motion rather than as drift.
inline constexpr std::uint16_t kMaxInterpolationSpan = 8;

// Where to draw `entity` at `alpha` in [0, 1) within the CURRENT tick period.
//
// The fraction actually blended is `(elapsed_ticks + alpha) / span_ticks`, so a value covering
// three ticks is drawn a third of the way along after one tick period, not all the way. With
// span 1 this reduces to `alpha` exactly, which is v1.
//
// Returns the current transform unchanged when there is no valid previous — the snap case above.
// `alpha` is clamped, so a caller that hands over a stale accumulator gets a sane frame rather than
// extrapolation it did not ask for; the composed fraction is clamped for the same reason.
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
