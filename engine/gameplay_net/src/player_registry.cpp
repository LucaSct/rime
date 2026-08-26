// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay_net/player_registry.hpp"

namespace rime::gameplay_net {

void PlayerRegistry::bind(net::SessionId id, ecs::Entity player) {
    if (!player.is_valid()) {
        return; // see the header: one spelling for "no player", not two
    }
    if (id.index >= rows_.size()) {
        rows_.resize(static_cast<std::size_t>(id.index) + 1);
    }
    Row& row = rows_[id.index];
    if (!row.bound) {
        ++live_;
    }
    row.generation = id.generation;
    row.player = player;
    row.bound = true;
}

void PlayerRegistry::forget(net::SessionId id) noexcept {
    if (id.index >= rows_.size()) {
        return;
    }
    Row& row = rows_[id.index];
    // The generation guard cuts both ways, and the second way is the one that matters: a
    // Disconnected event for the OLD incarnation must not evict the row a reconnected client has
    // already been given on the same slot. Reaping is ordered by the driver, so this is a
    // belt-and-braces check rather than a live race today — but it costs one compare and the class
    // of bug it forecloses is "the player who reconnected stops responding to input".
    if (!row.bound || row.generation != id.generation) {
        return;
    }
    row.bound = false;
    row.player = ecs::kNullEntity;
    --live_;
}

ecs::Entity PlayerRegistry::player_for(net::SessionId id) const noexcept {
    if (id.index >= rows_.size()) {
        return ecs::kNullEntity;
    }
    const Row& row = rows_[id.index];
    if (!row.bound || row.generation != id.generation) {
        return ecs::kNullEntity;
    }
    return row.player;
}

std::optional<net::SessionId> PlayerRegistry::session_for(ecs::Entity player) const noexcept {
    if (!player.is_valid()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.bound && row.player == player) {
            return net::SessionId{static_cast<std::uint32_t>(i), row.generation};
        }
    }
    return std::nullopt;
}

void PlayerRegistry::for_each(const std::function<void(net::SessionId, ecs::Entity)>& fn) const {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.bound) {
            fn(net::SessionId{static_cast<std::uint32_t>(i), row.generation}, row.player);
        }
    }
}

void PlayerRegistry::clear() noexcept {
    rows_.clear();
    live_ = 0;
}

} // namespace rime::gameplay_net
