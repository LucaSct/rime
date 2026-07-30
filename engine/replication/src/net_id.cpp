// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/net_id.hpp"

#include "rime/core/diagnostics/assert.hpp"

// NetId allocation and the NetId ↔ Entity maps. The header carries the design reasoning; this file
// is the mechanism, and it is deliberately the same mechanism as ecs::EntityDirectory's — a slot
// vector, a LIFO free list, and a generation stamp bumped on every release.
namespace rime::replication {

NetId NetIdAllocator::allocate() {
    ++live_count_;
    if (!free_.empty()) {
        const std::uint32_t index = free_.back();
        free_.pop_back();
        Slot& slot = slots_[index];
        slot.alive = true;
        // NB: the generation was already bumped by free(). Bumping on release rather than on
        // reuse means a handle goes stale the instant its referent dies, not later when something
        // happens to claim the slot — so a use-after-free is caught even if the index is never
        // reused at all.
        return NetId{index, slot.generation};
    }
    const auto index = static_cast<std::uint32_t>(slots_.size());
    slots_.push_back(Slot{/*generation=*/1, /*alive=*/true});
    // Generations start at 1, so a default-constructed NetId{kInvalidSlotIndex, 0} can never be
    // mistaken for a real one even if its index were somehow in range.
    return NetId{index, 1};
}

void NetIdAllocator::free(NetId id) noexcept {
    if (!is_live(id)) {
        return; // already dead, or never ours — releasing twice must not disturb the sequence
    }
    Slot& slot = slots_[id.index];
    slot.alive = false;
    ++slot.generation;
    free_.push_back(id.index);
    --live_count_;
}

NetId NetIdAllocator::live_id_at(std::uint32_t index) const noexcept {
    if (index >= slots_.size() || !slots_[index].alive) {
        return kNullNetId;
    }
    return NetId{index, slots_[index].generation};
}

bool NetIdAllocator::is_live(NetId id) const noexcept {
    if (id.index >= slots_.size()) {
        return false;
    }
    const Slot& slot = slots_[id.index];
    return slot.alive && slot.generation == id.generation;
}

void NetIdMap::bind(NetId net_id, ecs::Entity local) {
    RIME_ASSERT_MSG(net_id.is_valid(), "cannot bind the null NetId");
    if (net_id.index >= by_index_.size()) {
        by_index_.resize(static_cast<std::size_t>(net_id.index) + 1);
    }
    Slot& slot = by_index_[net_id.index];
    if (slot.bound) {
        // Replacing a live binding: drop the old reverse entry first, or it would outlive its
        // forward entry and answer net_id_of() with an id nothing resolves.
        by_entity_.erase(slot.local);
    }
    slot.generation = net_id.generation;
    slot.local = local;
    slot.bound = true;
    by_entity_[local] = net_id;
}

void NetIdMap::unbind(NetId net_id) noexcept {
    if (net_id.index >= by_index_.size()) {
        return;
    }
    Slot& slot = by_index_[net_id.index];
    // The generation guard matters here for the same reason it does in resolve(): a late-arriving
    // despawn for a *previous* incarnation of this index must not evict the successor that has
    // already been bound in its place.
    if (!slot.bound || slot.generation != net_id.generation) {
        return;
    }
    by_entity_.erase(slot.local);
    slot.bound = false;
    slot.local = ecs::kNullEntity;
}

ecs::Entity NetIdMap::resolve(NetId net_id) const noexcept {
    if (net_id.index >= by_index_.size()) {
        return ecs::kNullEntity;
    }
    const Slot& slot = by_index_[net_id.index];
    if (!slot.bound || slot.generation != net_id.generation) {
        return ecs::kNullEntity; // stale incarnation, or nothing bound yet — see the header
    }
    return slot.local;
}

std::optional<NetId> NetIdMap::net_id_of(ecs::Entity local) const {
    const auto it = by_entity_.find(local);
    if (it == by_entity_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void NetIdMap::for_each(const std::function<void(NetId, ecs::Entity)>& fn) const {
    // Iterating by_index_ (not by_entity_) is what makes the order NetId-ascending and therefore
    // identical on every peer — the property the convergence hash depends on, since local Entity
    // ordering differs across processes by construction.
    for (std::size_t i = 0; i < by_index_.size(); ++i) {
        const Slot& slot = by_index_[i];
        if (slot.bound) {
            fn(NetId{static_cast<std::uint32_t>(i), slot.generation}, slot.local);
        }
    }
}

void NetIdMap::clear() noexcept {
    by_index_.clear();
    by_entity_.clear();
}

} // namespace rime::replication
