// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "rime/core/byte_cursor.hpp"
#include "rime/destruction/damage_op.hpp"
#include "rime/destruction/world.hpp"
#include "rime/destruction_net/components.hpp"
#include "rime/destruction_net/wire.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/net_id.hpp"

namespace rime::physics {
class PhysicsWorld;
}

// DestructionClient (m11.4a) — the mirror half: decode the authority's damage-op batches, translate
// them into local instance ids, and hand them to the DestructionWorld.
//
// The client is purely a follower here, the same shape as ClientReplicator: there is no method that
// would let it author damage, and the instances it binds are marked Authority::Remote, which makes
// `apply_damage` a no-op on them and suppresses its own contact→damage conversion. Server authority
// expressed as an API shape rather than a rule someone has to remember.
//
// A TICK IS ATOMIC — AND SO IS THE GAP BETWEEN TWO TICKS. Both halves of this are load-bearing, and
// the second one is the half that is easy to miss.
//
// A tick's op list may span several packets, and every part of it must reach `apply_remote_ops`
// before the next `DestructionWorld::update()` runs. Applying part 1 at one update and part 2 at
// the next would put a support solve and a body swap BETWEEN two halves of one canonical sequence —
// the second half would then land on a wall of a different shape than the authority applied it to.
// So parts accumulate here and a batch is only released once complete.
//
// The converse is just as important and is why complete batches QUEUE rather than pile up together.
// Two of the server's ticks can easily arrive in one of the client's — a retransmit after a stall
// delivers a backlog all at once. Handing both to one `update()` merges them, and the fracture that
// belonged *between* them never happens: parts that should have detached in two waves detach in
// one. The alive bits and healths still converge (identical ops, identical order), which is exactly
// what makes this so quiet — what diverges is the DEBRIS COMPOSITION, which islands flew off
// together. That is visible on screen, and m11.4b addresses debris transforms by roster index, so a
// roster that two peers built differently is a wrong address rather than a cosmetic difference.
//
// So: one batch per destruction update, drained through `apply_next_batch`. A client that has
// fallen behind catches up by pumping more than one batch in a tick — each with its own update()
// between — never by merging them.
namespace rime::destruction_net {

class DestructionClient {
public:
    // Decode the destruction messages in an already-drained batch, translate them, and queue every
    // COMPLETE tick they finish. Call from PreSim. This only fills the queue — nothing reaches the
    // DestructionWorld until apply_next_batch below.
    //
    // Takes a drained span rather than the driver on purpose: `Session::drain_received` MOVES
    // messages out, so a session shared with replication has exactly one legitimate reader. The app
    // drains once and hands the same span to each subsystem (see replication/snapshot.hpp's tag
    // registry). Messages outside this module's tag block are left alone.
    void apply_messages(std::span<const net::Received> messages,
                        const replication::NetIdMap& map,
                        const ecs::World& world);

    // Convenience for a session that carries ONLY destruction traffic (tests, and a peer that does
    // not replicate ECS state). Drains the driver itself — do not mix with another drainer.
    void apply_inbound(net::NetDriver& driver,
                       const replication::NetIdMap& map,
                       const ecs::World& world);

    // Hand the OLDEST queued tick to the destruction world, and only that one. Returns false if
    // nothing was queued. Call once per `DestructionWorld::update()` — see the header note on why
    // never twice without an update() in between.
    //
    //   destruction_client.apply_next_batch(destruction);   // PreSim
    //   physics.step(dt);
    //   destruction.update(physics);                        // the fracture boundary this batch
    //   owns
    //
    // A client catching up after a stall loops that whole sequence while `pending_batches()`
    // remains high, which replays the authority's ticks one at a time, in order, at whatever rate
    // it can afford.
    bool apply_next_batch(destruction::DestructionWorld& destruction);

    // Complete ticks decoded but not yet handed over. Steady state is 0 or 1; a larger number means
    // this client is behind the authority by that many destruction ticks.
    [[nodiscard]] std::size_t pending_batches() const noexcept { return ready_.size(); }

    // Bind arriving debris mirrors to the chunks this client DERIVED for itself, and correct those
    // whose local simulation has drifted past `tolerance_m` from the authority's transform
    // (m11.4b). Call from PostSim, after the destruction update that may have created chunks.
    //
    // WHY A TOLERANCE RATHER THAN AN UNCONDITIONAL SNAP. Both peers derive the same chunks with the
    // same initial conditions, so the client's own physics is usually right and its motion is
    // continuous — which snapping every tick would destroy, replacing smooth tumbling rubble with a
    // stutter at exactly the moment the player is looking at it. The replicated transform is
    // authority for where a chunk ENDS UP, not a per-tick puppet string. Correct when the two have
    // actually parted company; otherwise let the local solver run.
    //
    // (Until m11.6 builds interpolation, a correction is a hard snap. That is the honest v1: the
    // tolerance keeps them rare, and smoothing them is interpolation's job, not something to fake
    // here with an ad-hoc lerp that would then have to be unpicked.)
    void sync_debris(const ecs::World& world,
                     const replication::NetIdMap& map,
                     destruction::DestructionWorld& destruction,
                     physics::PhysicsWorld& physics,
                     float tolerance_m = 0.25f);

    // How many of the authority's composition fingerprints this client's own rubble matched, and
    // how many it did not. A mismatch means the two peers' derivations have genuinely parted
    // company — the ordinal m11.4b addresses chunks by now names a DIFFERENT chunk, so a correction
    // would move the wrong rubble. Detection only in v1; see composition.hpp on why repair waits
    // for the late-join machinery rather than being guessed at here.
    [[nodiscard]] std::uint64_t composition_matches() const noexcept {
        return composition_matches_;
    }

    [[nodiscard]] std::uint64_t composition_mismatches() const noexcept {
        return composition_mismatches_;
    }

    // Debris mirrors successfully bound to a locally-derived chunk.
    [[nodiscard]] std::uint64_t debris_bound() const noexcept { return debris_bound_; }

    // Chunks snapped to the authority's transform because local simulation had drifted past the
    // tolerance. A proof asserts on this: zero corrections means the correction path was never
    // exercised, and the test proved only that two identical simulations stayed identical.
    [[nodiscard]] std::uint64_t debris_corrections() const noexcept { return debris_corrections_; }

    // Debris mirrors whose `DebrisOrigin` named a chunk this client does not have. Expected to be
    // transient — the source destructible's own mirror, or the op batch that produced the chunk,
    // can still be in flight. Persistently non-zero means the two rosters have genuinely diverged,
    // which is what the composition hash is for.
    [[nodiscard]] std::uint64_t debris_unresolved() const noexcept { return debris_unresolved_; }

    // Counters — a loss/reorder proof in which nothing was ever deferred or dropped proves nothing
    // (the m11.1 harness discipline).
    [[nodiscard]] std::uint64_t ticks_applied() const noexcept { return ticks_applied_; }

    [[nodiscard]] std::uint64_t ops_applied() const noexcept { return ops_applied_; }

    [[nodiscard]] std::uint64_t multipart_ticks() const noexcept { return multipart_ticks_; }

    // Ops naming a NetId this client cannot resolve, or an entity it has not bound yet — the
    // cross-channel race (a mirror's Spawn is reliable, but binding it also needs its
    // LocalTransform, which rides the unreliable delta path and can lag). Expected to be non-zero
    // in any test with real loss; a zero there means the interesting path was never exercised.
    [[nodiscard]] std::uint64_t ops_dropped_unmapped() const noexcept { return dropped_unmapped_; }

    [[nodiscard]] std::uint64_t malformed_messages() const noexcept { return malformed_; }

    // Ops of a tick still waiting for their sibling packets. Non-zero only mid-tick.
    [[nodiscard]] std::size_t pending_parts() const noexcept { return pending_.size(); }

private:
    void on_damage_ops(core::ByteReader& reader,
                       const replication::NetIdMap& map,
                       const ecs::World& world);

    void on_composition_check(core::ByteReader& reader);

    // Compare the authority's fingerprints against our own rubble. Called from sync_debris, which
    // is the point in the tick where the comparison is meaningful.
    void verify_composition(const ecs::World& world,
                            const replication::NetIdMap& map,
                            const destruction::DestructionWorld& destruction);

    void flush();

    // One destructible's expected debris shape, as the authority left it after a given batch.
    struct ExpectedComposition {
        replication::NetId source{};
        std::uint64_t hash = 0;
    };

    // One completed tick, waiting for its own update(), and the fingerprints that describe the
    // state it should leave behind. The check travels WITH its batch rather than in a single slot
    // on the client, because one drained span routinely carries several ticks: a lone slot would
    // keep only the newest, silently discarding the checks for every batch still queued behind it —
    // and those are exactly the batches most likely to be the ones that went wrong.
    struct Batch {
        std::uint64_t tick = 0;
        std::vector<destruction::DamageOp> ops;
        std::vector<ExpectedComposition> expected;
    };

    // The tick currently being ACCUMULATED, and the ops gathered for it so far. Only ONE tick is
    // ever mid-accumulation: the channel is ordered, so a part of tick T+1 cannot precede a part of
    // T. `pending_seen_` is a bitmask over part indices — 64 bits covers kMaxPartsPerTick exactly,
    // so completeness is tested the same way whatever the split, and a duplicated part cannot
    // complete a tick that is still missing one.
    std::uint64_t pending_tick_ = 0;
    std::uint64_t pending_seen_ = 0;
    std::uint8_t pending_count_ = 0;
    std::vector<destruction::DamageOp> pending_;

    // Completed ticks awaiting their own fracture boundary, oldest first. A deque because it is
    // drained from the front and filled at the back, and the ordering IS the contract.
    std::deque<Batch> ready_;

    // No translation table on this side: the client goes NetId → entity (NetIdMap) → InstanceId,
    // and that last hop is a direct `DestructibleInstanceRef` component read. Only the server needs
    // a built table, because it walks the mapping backwards.
    std::vector<net::Received> inbox_;

    std::uint64_t ticks_applied_ = 0;
    std::uint64_t ops_applied_ = 0;
    std::uint64_t multipart_ticks_ = 0;
    std::uint64_t dropped_unmapped_ = 0;
    std::uint64_t malformed_ = 0;
    std::uint64_t debris_bound_ = 0;
    std::uint64_t debris_corrections_ = 0;
    std::uint64_t debris_unresolved_ = 0;
    std::uint64_t composition_matches_ = 0;
    std::uint64_t composition_mismatches_ = 0;

    // The fingerprints belonging to the batch most recently released by apply_next_batch, awaiting
    // the fracture boundary that makes them comparable.
    std::vector<ExpectedComposition> pending_verify_;
};

} // namespace rime::destruction_net
