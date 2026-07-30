// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "rime/destruction/ids.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/world.hpp"

namespace rime::physics {
class PhysicsWorld;
}

// The destructible BIND system — "an entity that says it is a destructible becomes one".
//
// M8.2 defined the authoring surface (`Destructible{asset}` as intent, `DestructibleInstanceRef` as
// the runtime link) and deferred the system that closes the gap; through M8 everything drove
// `DestructionWorld::spawn` directly, and `DestructibleInstanceRef` was written by nothing at all.
// m11.4 needs it, because it is what makes a destructible ADDRESSABLE ACROSS THE WIRE.
//
// WHY REPLICATION FORCED THE ISSUE. A damage op names an instance, but `InstanceId` is a local
// table index: two peers agree on it only if they happened to spawn their instances in the same
// order, which late-join breaks immediately and permanently. The fix is to name the ENTITY instead
// and let m11.3's existing NetId machinery carry it — the server replicates the destructible's
// entity, the client's mirror arrives with the same `Destructible{asset}` component, and each side
// independently binds it to whatever local InstanceId it likes. The wire never learns either one.
// So the bind table IS the address translation, and building it here — in the destruction module,
// with no knowledge of networking — keeps the seam where it belongs.
namespace rime::destruction {

// Resolve a cooked asset's content id to a pattern already registered in the DestructionWorld.
//
// Binding deliberately does NOT load or register patterns: `register_pattern` is the cold path that
// pays for hull and compound registration, it needs a loaded `assets::DestructibleAsset`, and doing
// it implicitly inside a per-tick system would hide that cost exactly where it is least affordable.
// The caller registers patterns up front and hands over the lookup. Return a null PatternId for an
// asset this peer does not have; the entity is then left unbound and retried on a later tick, which
// is the behaviour a streaming asset system wants.
using PatternResolver = std::function<PatternId(std::uint64_t asset)>;

// How a bind pass went. `bound` is the useful number; `unresolved` counts entities whose asset the
// resolver did not know, which is the difference between "nothing to do" and "the content is
// missing" — a distinction worth having when a wall silently fails to appear.
struct BindStats {
    std::size_t bound = 0;
    std::size_t unresolved = 0;
};

// Stand up every entity that has `Destructible` but is not yet bound, and record the link in a
// `DestructibleInstanceRef`. Call once per tick from PreSim — after replication has applied its
// spawns, so a mirror that arrived this tick is standing before anything tries to damage it.
//
// Idempotent by construction: an entity with a live `DestructibleInstanceRef` is skipped, so
// re-running the pass costs one query and binds nothing. The instance is placed at the entity's
// `WorldTransform` when it has one (the propagated placement — a destructible may be parented) and
// its `LocalTransform` otherwise; an entity with neither is placed at the origin, which is the same
// thing `spawn` would have been passed by a caller that did not care.
//
// `authority` is stamped on every instance this pass creates. A server binds `Local` (it owns them,
// and both damage sources feed them); a client binds `Remote`, which is what stops its own solver's
// contact impulses from eroding a wall the server is already eroding for it.
BindStats bind_destructibles(ecs::World& world,
                             DestructionWorld& destruction,
                             physics::PhysicsWorld& physics,
                             const PatternResolver& resolve,
                             Authority authority = Authority::Local);

// Build the INVERSE of the bind table: `out[i]` is the entity bound to instance index `i`, or a
// null entity where nothing is. Sized to the highest bound index; `out` is cleared first.
//
// The networking layer needs this direction — an op names an InstanceId and must be translated to
// the entity whose NetId goes on the wire. Built once per tick and indexed per op, rather than
// searched per op: a collapsing wall commits hundreds of ops in a tick, and a linear scan of the
// world inside that loop is the difference between a translation pass you never notice and one that
// shows up in a profile shaped exactly like the destruction it is trying to replicate.
void build_instance_entity_table(const ecs::World& world, std::vector<ecs::Entity>& out);

} // namespace rime::destruction
