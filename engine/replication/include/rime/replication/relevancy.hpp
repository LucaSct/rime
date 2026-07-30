// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rime/core/math/vec.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/net/session.hpp"
#include "rime/replication/server_replicator.hpp"

// Ready-made relevancy policies (m11.5). A game with unusual needs — team-based, portal-based, PVS
// — writes its own `RelevancyFn` and never includes this header. This is the policy almost everyone
// wants first, written once so it is written correctly once.
//
// WHY A FACTORY RATHER THAN A FUNCTION. `distance_relevancy` returns a `RelevancyFn` instead of
// being one, because the policy needs to remember things between calls (see hysteresis below) and a
// bare function has nowhere to keep them. The returned callable owns its state and is safe to hand
// to `ServerReplicator::set_relevancy`.
namespace rime::replication {

// Where a client is looking from. Returns false if this client has no viewpoint yet — a session
// that has connected but whose pawn does not exist, which is the normal state for the first few
// ticks and stays the normal state until M12 builds a player controller at all.
//
// Deliberately a callback rather than a position table the replicator owns: the engine has no
// opinion about what a "viewpoint" is. A shooter uses the camera, a strategy game uses the screen
// centre, a dedicated server replaying a demo uses whatever the demo says. None of those belong in
// `engine/replication`.
using ViewpointFn = std::function<bool(net::SessionId, core::Vec3& out_position)>;

// Configuration for `distance_relevancy`.
struct DistanceRelevancy {
    // Required. Without it every client is viewpointless and the policy degrades to "everything is
    // relevant", which is safe but pointless.
    ViewpointFn viewpoint;

    // Entities within this distance of the viewpoint are relevant. In world units.
    float radius = 100.0f;

    // How much further than `radius` an ALREADY-relevant entity may drift before it is dropped, as
    // a fraction of the radius. 0.1 means enter at 100 m, leave at 110 m.
    //
    // WHY THIS IS NOT OPTIONAL DECORATION. Without hysteresis, an entity hovering on the boundary
    // flips relevant/irrelevant on alternating ticks. Each flip back to relevant is an ENTRY, and
    // an entry is expensive twice over: the entity's full state is re-sent (it has not changed, so
    // the ordinary delta would skip it — that is the whole reason entries are forced), and
    // `publish_delta` gives up its per-chunk "changed since baseline" skip for that entire tick,
    // for that client. One jittering chunk of rubble is enough to make every tick a full walk.
    //
    // Costing a boundary in an engine that budgets bandwidth is exactly the case where the naive
    // version is worst: the entity least worth sending is the one furthest away, and it is
    // precisely the far ones that sit on the boundary.
    float hysteresis = 0.1f;
};

// Priority given to an entity the policy cannot locate — above every positioned entity, so it sorts
// first when the budget binds.
//
// A positioned entity scores in (0, 1]; this is deliberately larger than any of them.
inline constexpr float kUnpositionedPriority = 2.0f;

// Build a nearest-first distance policy over `world`.
//
// `world` must outlive the returned function, which holds a reference to it. Install it with
// `ServerReplicator::set_relevancy`.
//
// THE POSITION SOURCE, AND THE ONE DECISION IN HERE THAT IS NOT OBVIOUS. An entity's position is
// read from `WorldTransform` if it has one, otherwise from `LocalTransform`. World first because it
// is the absolute placement and the only correct answer for a parented entity; Local as the
// fallback because a root's local placement IS its world placement, and debris — the single largest
// population this policy exists to cull — are spawned as roots carrying only a `LocalTransform`
// (destruction_net/src/destruction_server.cpp). A policy that insisted on `WorldTransform` would
// silently fail to locate the exact entities it was built for.
//
// AN ENTITY WITH NEITHER IS ALWAYS RELEVANT, at `kUnpositionedPriority`. This is the
// safety-critical default and it is worth being explicit about why, because the convenient answer
// is the opposite one. A distance policy that culled what it could not measure would silently stop
// replicating every non-spatial entity in the game — scores, match state, team rosters, anything a
// designer adds later that happens to have no transform — and it would do so invisibly, because the
// entities most likely to be unpositioned are the ones nobody thinks of as "in the world". Failing
// open costs bandwidth; failing closed costs correctness, and costs it quietly. When a policy
// cannot answer a question, it must not pretend the answer was "no".
//
// THE CURVE. Priority is `radius / (radius + distance)`: 1.0 at the viewpoint, 0.5 at the radius,
// strictly decreasing, and never zero. That last property matters more than it looks. The relevance
// DECISION is a separate comparison against the radius, not "did the priority reach zero" — so
// nothing lands on the `<= 0 means irrelevant` boundary by arithmetic accident, and an entity
// sitting exactly at the radius has a well-defined, comfortably positive priority rather than a
// float that flickers around zero as the last bit wobbles.
[[nodiscard]] RelevancyFn distance_relevancy(const ecs::World& world, DistanceRelevancy config);

} // namespace rime::replication
