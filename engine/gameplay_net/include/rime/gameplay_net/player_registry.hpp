// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "rime/ecs/entity.hpp"
#include "rime/net/session.hpp"

// Who is driving which avatar (m12.3, ADR-0035 §3).
namespace rime::gameplay_net {

// The session ↔ player-entity table. One per server.
//
// It is the smallest possible thing and it is still worth its own file, because it is the third
// per-session record in this codebase and the previous two both shipped the same bug. `SessionId`
// is a RECYCLABLE SLOT (net/session.hpp: "a crashed client's reincarnation recycles the slot"), and
// a per-session record keyed by that slot must not outlive its subject — corollary 2 of
// docs/design/replication.md, which has already produced one bug in `replication` (instance six)
// and one in `ServerInputReceiver`. The mechanism here is the same as theirs and so is the
// remedy: the table is indexed by `SessionId::index`, every row stores the GENERATION it was
// written for, and a lookup whose generation does not match is a miss rather than a hit on a
// stranger's avatar.
//
// `forget` alone would have been a leak wearing an API's clothes — correct, and called by nobody —
// so `on_session_events` is what the server actually calls, and it is what reaps disconnections.
class PlayerRegistry {
public:
    // Bind a session to the entity it drives. Re-binding a live session REPLACES the entry (the
    // caller is expected to have retired the old avatar first); binding a null entity is a no-op,
    // because a table entry naming nothing is indistinguishable from an absent one and having both
    // spellings is how a "does this session have a player" test starts disagreeing with itself.
    void bind(net::SessionId id, ecs::Entity player);

    // Release one session's row. A no-op for an unknown or stale id.
    void forget(net::SessionId id) noexcept;

    // The entity `id` drives, or kNullEntity if it drives nothing (unknown, stale, or reaped).
    [[nodiscard]] ecs::Entity player_for(net::SessionId id) const noexcept;

    // The session driving `player`, or nullopt if nobody does.
    //
    // An OPTIONAL rather than a sentinel, deliberately, and the reason is a live trap rather than
    // taste: `net::SessionId{}` is `{index 0, generation 0}`, and the driver hands out exactly that
    // handle for the first session on a fresh server (net_driver.cpp allocates slot 0 with
    // generation 0 and only bumps on reap). So the obvious "return a default-constructed id for
    // not-found" would make "nobody drives this entity" indistinguishable from "the first client
    // does" — an off-by-one-client bug that a two-peer test could never catch, because the answer
    // is only wrong for the peer a two-peer test has exactly one of. `NetIdMap::net_id_of` is the
    // in-tree precedent for the same call.
    //
    // The reverse direction is a LINEAR SCAN by choice: it is asked once per shot for attribution,
    // never per tick per entity, and a second index would be a second thing to keep consistent for
    // a lookup that is not hot. If a future brick makes it hot, the fix is a hash map and this
    // comment is the note saying so.
    [[nodiscard]] std::optional<net::SessionId> session_for(ecs::Entity player) const noexcept;

    // Every live binding, in ascending session-index order (stable within a run, which is what a
    // deterministic per-player walk needs — see the consume loop's ordering note).
    void for_each(const std::function<void(net::SessionId, ecs::Entity)>& fn) const;

    [[nodiscard]] std::size_t size() const noexcept { return live_; }

    void clear() noexcept;

private:
    struct Row {
        std::uint32_t generation = 0;
        ecs::Entity player = ecs::kNullEntity;
        bool bound = false;
    };

    std::vector<Row> rows_; // indexed by SessionId::index
    std::size_t live_ = 0;
};

} // namespace rime::gameplay_net
