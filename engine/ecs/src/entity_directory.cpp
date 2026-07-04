// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/entity_directory.hpp"

#include "rime/core/containers/handle.hpp"
#include "rime/core/diagnostics/assert.hpp"

namespace rime::ecs {

Entity EntityDirectory::allocate() {
    std::uint32_t index;
    if (!free_.empty()) {
        // Reuse a recently-freed slot (LIFO keeps it hot in cache). Its generation was already
        // bumped when it was freed, so the id we hand out cannot collide with any still-outstanding
        // stale one.
        index = free_.back();
        free_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(slots_.size());
        // The sentinel value can never be a live index (a null Entity uses it), so exhausting the
        // 2^32-1 index space is a hard error, not a silently-aliasing wrap.
        RIME_ASSERT(index != core::kInvalidSlotIndex);
        slots_.push_back(Slot{});
    }
    Slot& s = slots_[index];
    s.alive = true;
    s.location = EntityLocation{}; // "unplaced" until the archetype layer sets it (M4.3)
    ++alive_count_;
    return Entity{index, s.generation};
}

bool EntityDirectory::free(Entity e) noexcept {
    if (!valid(e)) {
        return false; // already stale/invalid — freeing twice is a no-op, not a crash
    }
    Slot& s = slots_[e.index];
    s.alive = false;
    ++s.generation; // every Entity that still points here is now a generation behind == stale
    free_.push_back(e.index);
    --alive_count_;
    return true;
}

EntityLocation* EntityDirectory::location(Entity e) noexcept {
    return valid(e) ? &slots_[e.index].location : nullptr;
}

const EntityLocation* EntityDirectory::location(Entity e) const noexcept {
    return valid(e) ? &slots_[e.index].location : nullptr;
}

void EntityDirectory::reserve(std::size_t n) {
    slots_.reserve(n);
}

void EntityDirectory::clear() noexcept {
    // Bump the generation of every live slot so outstanding handles go stale, then return all
    // indices to the free list. Already-free slots kept their (already-bumped) generation, so their
    // old handles remain stale too.
    for (auto& s : slots_) {
        if (s.alive) {
            s.alive = false;
            ++s.generation;
        }
    }
    free_.clear();
    free_.reserve(slots_.size());
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        free_.push_back(i);
    }
    alive_count_ = 0;
}

} // namespace rime::ecs
