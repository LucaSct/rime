// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/ecs/entity.hpp"

// The EntityDirectory is the ECS's source of truth for "which entities exist, and where does each
// one's data live." It is a flat table indexed *directly* by an entity's directory index
// (ADR-0018):
//
//     slots_[index] = { generation, alive, location }
//
// Allocating an entity hands out a free index (reused LIFO from a free list, else a fresh one) with
// that slot's current generation. Freeing bumps the generation and returns the index to the free
// list — so any Entity still holding the old generation is detected as stale. This is the same
// generational-recycling idea as the SlotMap (core/containers/slot_map.hpp), but deliberately *not*
// a SlotMap: an entity lookup must be a single direct index into `slots_` with no slot→dense
// indirection, because the dense hot path in this engine is iterating ARCHETYPES (M4.3), not the
// directory. The directory only answers random-access questions — "is this entity alive? where does
// it live?" — so a flat, directly-indexed table is exactly the right shape, and the SlotMap's
// dense-compaction machinery would be unused weight here.
//
// The `location` field (which archetype / chunk / row holds the entity's components) is written by
// the archetype-storage layer in M4.3; in M4.1 it is reserved and left at its default. That is the
// honest state of this brick: the entity lifecycle and generational safety are real and tested now;
// the location wiring lands with the storage it points into.
//
// Threading: like all of the world's structural operations, allocate/free are main-thread only
// (they mutate shared bookkeeping). Parallel systems (M4.4) read/write component *data*, never
// restructure.
//
// Note: backing storage is std::vector for now — the same deliberate later seam the SlotMap calls
// out. Drawing the directory (and, more importantly, the M4.2 chunks) from core's allocators is
// when that module becomes load-bearing; the algorithm here is the point.
namespace rime::ecs {

// Where an entity's components live. Populated by the archetype layer (M4.3); the default value
// means "unplaced" (no archetype yet), which is every entity's state in M4.1.
struct EntityLocation {
    std::uint32_t archetype = 0xFFFFFFFFu; // index into World's archetype list; 0xFFFFFFFF = none
    std::uint32_t chunk = 0;               // which chunk within the archetype
    std::uint32_t row = 0;                 // which row within the chunk
};

class EntityDirectory {
public:
    // Create a new entity: reuse a recycled index if one is free (LIFO keeps it cache-hot), else
    // grow the table by one. The returned Entity carries the slot's current generation.
    [[nodiscard]] Entity allocate();

    // Destroy an entity. Bumps its slot generation (invalidating every outstanding Entity to it)
    // and returns the index to the free list. Returns false if `e` was already stale/invalid.
    bool free(Entity e) noexcept;

    // True iff `e` names a slot that is still live AND carries the matching generation.
    [[nodiscard]] bool is_alive(Entity e) const noexcept { return valid(e); }

    // The entity's location record, or nullptr if `e` is stale/invalid. The mutable overload lets
    // the storage layer update the record when an entity moves between archetypes (M4.3).
    [[nodiscard]] EntityLocation* location(Entity e) noexcept;
    [[nodiscard]] const EntityLocation* location(Entity e) const noexcept;

    // Number of currently-live entities.
    [[nodiscard]] std::size_t alive_count() const noexcept { return alive_count_; }

    // Number of directory slots ever created (live + recycled). Mostly for tests / diagnostics.
    [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }

    // Hint the expected peak entity count to avoid reallocations while spawning.
    void reserve(std::size_t n);

    // Destroy all entities: every live slot's generation is bumped (existing handles go stale) and
    // all indices return to the free list. The directory stays usable afterward.
    void clear() noexcept;

private:
    struct Slot {
        std::uint32_t generation = 0;
        bool alive = false;
        EntityLocation location{};
    };

    // The one liveness predicate shared by is_alive/location: in range, alive, matching generation.
    [[nodiscard]] bool valid(Entity e) const noexcept {
        return e.index < slots_.size() && slots_[e.index].alive &&
               slots_[e.index].generation == e.generation;
    }

    std::vector<Slot> slots_;         // sparse table, indexed directly by Entity::index
    std::vector<std::uint32_t> free_; // recycled indices, LIFO
    std::size_t alive_count_ = 0;
};

} // namespace rime::ecs
