// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/destruction/world.hpp"
#include "rime/ecs/world.hpp"
#include "rime/replication/net_id.hpp"

// The composition check (m11.4b) — the mechanism that notices when two peers' rubble has stopped
// being the same rubble.
//
// WHAT IT GUARDS. m11.4b addresses a chunk by its ORDINAL among its instance's debris in creation
// order, because both peers derive that sequence identically from the same op stream applied at the
// same fracture boundaries. That is a strong property and m11.4a proves it holds — but it is a
// property, not a guarantee, and when it fails it fails in the worst possible way: the ordinal
// still resolves, to a DIFFERENT chunk. The client then confidently corrects the wrong piece of
// rubble toward another piece's position, and every later correction makes it worse. A silent wrong
// answer is exactly the failure mode worth spending bytes to detect (ADR-0033 A12 is the concrete
// way this went wrong once already).
//
// WHY A HASH AND NOT THE COMPOSITION ITSELF. Sending the membership would cost far more than the op
// stream that produced it, and would defeat the point — composition is derived precisely so it does
// not have to be sent. A hash is a check on a shared derivation, not a transmission of it.
//
// WHAT DETECTION BUYS TODAY, HONESTLY. It counts and reports. Automatic REPAIR needs either a
// client→server request path or a periodic authoritative state broadcast, neither of which exists
// yet — both are late-join machinery, and building half of it here would be guessing at the shape
// of a brick that has not been designed. Until then a mismatch is a loud, countable signal that the
// derivation broke, and the A3 state-application seam is the manual repair.
namespace rime::destruction_net {

// Fold one instance's debris composition — which parts left together, in the instance's own
// creation order — into a 64-bit fingerprint. FNV-1a, field by field, never over a padded struct,
// matching the discipline of `DestructionWorld::state_hash` and `PhysicsWorld::world_hash`.
//
// Deliberately folds ONLY peer-independent facts. No physics body ids (allocation-order artifacts
// two independently-built worlds never agree on — the trap that makes `state_hash` a same-process
// witness only), no local instance or roster indices, no transforms (those diverge by design and
// are what the replication corrects). Just the shape of the break.
[[nodiscard]] std::uint64_t
debris_composition_hash(const destruction::DestructionWorld& destruction,
                        destruction::InstanceId instance) noexcept;

// The CROSS-PEER destruction witness: one number two networked peers must agree on, folded over
// every replicated destructible in NetId order — per-part alive bits and health, plus each
// instance's debris composition.
//
// WHY THIS IS NOT `DestructionWorld::state_hash()`. That one is the M8 REPLAY witness and folds
// physics BODY IDS. Body ids are allocation-order artifacts of a single process: two peers build
// their physics worlds independently and populate them differently (a client under m11.5's
// relevancy holds a subset), so their ids agree about nothing even when the destruction is
// identical. It is the right witness for "this process replayed its own tick" and the wrong one for
// "these two processes agree" — and reaching for it across a wire yields a mismatch on every tick,
// which reads as a broken engine rather than as the wrong question.
//
// WHY IT WALKS NetId ORDER. Local `Entity` and `InstanceId` orders are private to each peer. NetId
// is the only name both share, and `NetIdMap::for_each` is specified to walk it ascending on both
// sides.
//
// WHY IT LIVES IN THE ENGINE RATHER THAN IN A TEST. It is the thing a dedicated server compares to
// spot a diverged client, the thing m11.7's scripted match hash-verifies in CI, and the thing a
// sample prints to show two windows are showing the same wall. Any of those re-deriving it
// privately would be three subtly different answers to one question — and a witness that different
// callers compute differently is not a witness.
//
// Entities in `map` that are not bound destructibles are skipped, so this is safe to call on a
// world replicating anything else alongside its rubble.
[[nodiscard]] std::uint64_t shared_state_hash(const ecs::World& world,
                                              const replication::NetIdMap& map,
                                              const destruction::DestructionWorld& destruction);

} // namespace rime::destruction_net
