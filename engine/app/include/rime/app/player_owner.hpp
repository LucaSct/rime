// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/app/seat_table.hpp"
#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"

// `PlayerOwner` (m20.8b2, ADR-0108 §4) — which SEAT an entity belongs to, as a replicated
// component.
//
// The problem it solves is the client's: a multi-seat mirror holds every player's body, and "the
// kinematic capsule" (hello-game's one-seat lookup) now matches several. The server knows whose
// body is whose -- it spawned each one in `on_player_joined(slot)` -- so it writes the slot ONTO
// the entity, the world stream carries it like any other field, and the client, told its seat by
// the codec-3 hello reply, finds its own body by matching the pair. No game-specific lookup and no
// new wire message: the replication the mirror already has is the delivery.
//
// The GENERATION is part of the match, not decoration. A seat vacated and re-taken keeps its
// index, so for the tick or two in which the departed player's body and the newcomer's are both in
// a mirror (the retraction and the spawn travel separately), index alone would find the wrong one
// -- the same stale-handle bug SeatTable's generations exist to stop.
//
// The fields are u32 because reflection carries 4- and 8-byte integers; the values are
// `PlayerSlot`'s u16s. An engine type in `rime::app` because the client engine reads it too (its
// finished report lists every seated body's pose); a game registers it in its own component set,
// as it registers the transform components.
namespace rime::app {

struct PlayerOwner {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] static PlayerOwner of(PlayerSlot slot) noexcept {
        return {slot.index, slot.generation};
    }

    [[nodiscard]] PlayerSlot slot() const noexcept {
        return {static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(generation)};
    }

    [[nodiscard]] bool owns(PlayerSlot slot) const noexcept {
        return index == slot.index && generation == slot.generation;
    }
};

// Every entity in `world` whose PlayerOwner names exactly `slot`, handed to `visit` -- a game
// marking both a body and its projectiles with an owner picks the body by its other components.
// Linear: called once per join and once per client bind, never per tick.
template <class Visit> void for_each_owned_by(ecs::World& world, PlayerSlot slot, Visit&& visit) {
    world.query<PlayerOwner>().for_each([&](ecs::Entity entity, const PlayerOwner& owner) {
        if (owner.owns(slot))
            visit(entity);
    });
}

} // namespace rime::app

RIME_REFLECT_BEGIN(rime::app::PlayerOwner)
RIME_REFLECT_FIELD(index)
RIME_REFLECT_FIELD(generation)
RIME_REFLECT_END()
