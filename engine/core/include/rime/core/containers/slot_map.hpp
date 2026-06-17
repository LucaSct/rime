// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "rime/core/containers/handle.hpp"
#include "rime/core/diagnostics/assert.hpp"

// A SlotMap: O(1) insert / erase / lookup with stable Handles and *cache-friendly iteration*.
//
// The trick is a layer of indirection split into two arrays:
//   - a DENSE array `dense_` holding the values packed with no gaps, so iterating all live
//     elements is a tight linear scan (what the ECS and renderers want every frame); and
//   - a SPARSE `slots_` table indexed by Handle::index, each slot storing a `generation` stamp
//     and the position of its value in the dense array.
// A handle therefore names a *slot*, and the slot redirects to wherever the value currently
// lives in the dense array. Erase uses swap-and-pop: the last dense element is moved into the
// hole and its owning slot is repointed, keeping the dense array gap-free in O(1). Freed slots
// bump their generation and go on a free list for reuse — so a stale handle (old generation)
// is rejected rather than reading a recycled slot's new occupant.
//
// Note: backing storage is std::vector for now (RAII, correct, clear). Making it draw from the
// engine's allocators (M1.2) is a deliberate later seam — the algorithm here is the point.
namespace rime::core {

template <class T> class SlotMap {
public:
    using HandleType = Handle<T>;

    // ---- capacity ----
    [[nodiscard]] std::size_t size() const noexcept { return dense_.size(); }

    [[nodiscard]] bool empty() const noexcept { return dense_.empty(); }

    void reserve(std::size_t n) {
        slots_.reserve(n);
        dense_.reserve(n);
        dense_to_slot_.reserve(n);
    }

    // ---- insert ----
    // Takes the value by value and moves it into dense storage (works for both lvalues and
    // rvalues at the call site). Returns a handle naming the new element.
    HandleType insert(T value) {
        const std::uint32_t slot_index = acquire_slot();
        Slot& slot = slots_[slot_index];
        slot.dense_index = static_cast<std::uint32_t>(dense_.size());
        dense_.push_back(std::move(value));
        dense_to_slot_.push_back(slot_index);
        return HandleType{slot_index, slot.generation};
    }

    // In-place construction, avoiding a temporary.
    template <class... Args> HandleType emplace(Args&&... args) {
        const std::uint32_t slot_index = acquire_slot();
        Slot& slot = slots_[slot_index];
        slot.dense_index = static_cast<std::uint32_t>(dense_.size());
        dense_.emplace_back(std::forward<Args>(args)...);
        dense_to_slot_.push_back(slot_index);
        return HandleType{slot_index, slot.generation};
    }

    // ---- lookup ----
    // True only if the handle names a slot that is still live AND carries the matching
    // generation — i.e. it has not been erased (and the slot possibly reused) since it was issued.
    [[nodiscard]] bool contains(HandleType h) const noexcept {
        return h.index < slots_.size() && slots_[h.index].dense_index != kInvalidSlotIndex &&
               slots_[h.index].generation == h.generation;
    }

    // Returns a pointer to the value, or nullptr if the handle is stale/invalid. Pointer is valid
    // until the next insert/erase (the dense array may relocate or swap) — hold the handle, not
    // the pointer.
    [[nodiscard]] T* get(HandleType h) noexcept {
        return contains(h) ? &dense_[slots_[h.index].dense_index] : nullptr;
    }

    [[nodiscard]] const T* get(HandleType h) const noexcept {
        return contains(h) ? &dense_[slots_[h.index].dense_index] : nullptr;
    }

    // ---- erase ----
    // Removes the element (swap-and-pop to keep dense_ packed), bumps the slot's generation so
    // outstanding handles to it become stale, and frees the slot for reuse. Returns false if the
    // handle was already invalid.
    bool erase(HandleType h) noexcept {
        if (!contains(h)) {
            return false;
        }
        Slot& slot = slots_[h.index];
        const std::uint32_t hole = slot.dense_index;
        const std::uint32_t last = static_cast<std::uint32_t>(dense_.size() - 1);

        // Move the last dense element into the hole and repoint its owning slot, unless the
        // erased element already was the last one.
        if (hole != last) {
            dense_[hole] = std::move(dense_[last]);
            const std::uint32_t moved_slot = dense_to_slot_[last];
            dense_to_slot_[hole] = moved_slot;
            slots_[moved_slot].dense_index = hole;
        }
        dense_.pop_back();
        dense_to_slot_.pop_back();

        slot.dense_index = kInvalidSlotIndex; // mark the slot free
        ++slot.generation;                    // invalidate every handle that pointed here
        free_slots_.push_back(h.index);
        return true;
    }

    // Removes all elements. Live slots have their generation bumped (so existing handles go
    // stale) and every slot is returned to the free list.
    void clear() noexcept {
        dense_.clear();
        dense_to_slot_.clear();
        free_slots_.clear();
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].dense_index != kInvalidSlotIndex) {
                slots_[i].dense_index = kInvalidSlotIndex;
                ++slots_[i].generation;
            }
            free_slots_.push_back(i);
        }
    }

    // ---- iteration over the packed values (the hot path) ----
    [[nodiscard]] T* begin() noexcept { return dense_.data(); }

    [[nodiscard]] T* end() noexcept { return dense_.data() + dense_.size(); }

    [[nodiscard]] const T* begin() const noexcept { return dense_.data(); }

    [[nodiscard]] const T* end() const noexcept { return dense_.data() + dense_.size(); }

    [[nodiscard]] T* data() noexcept { return dense_.data(); }

    [[nodiscard]] const T* data() const noexcept { return dense_.data(); }

    // Visit every live (handle, value) pair. The handle is reconstructed from the dense->slot
    // back-pointer plus the slot's current generation — useful when a system needs to remember
    // or hand back references while iterating.
    template <class Fn> void for_each(Fn&& fn) {
        for (std::size_t i = 0; i < dense_.size(); ++i) {
            const std::uint32_t slot_index = dense_to_slot_[i];
            fn(HandleType{slot_index, slots_[slot_index].generation}, dense_[i]);
        }
    }

private:
    struct Slot {
        std::uint32_t generation = 0;
        std::uint32_t dense_index = kInvalidSlotIndex; // kInvalidSlotIndex => slot is free
    };

    // Reuse a freed slot if one exists (LIFO keeps recently-freed slots hot in cache), else grow
    // the sparse table by one. Returns the slot index to populate.
    std::uint32_t acquire_slot() {
        if (!free_slots_.empty()) {
            const std::uint32_t index = free_slots_.back();
            free_slots_.pop_back();
            return index;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(slots_.size());
        RIME_ASSERT(index != kInvalidSlotIndex); // 2^32-1 slots: the index space is exhausted
        slots_.push_back(Slot{});
        return index;
    }

    std::vector<Slot> slots_;                  // sparse, indexed by Handle::index
    std::vector<std::uint32_t> free_slots_;    // indices of free slots (LIFO)
    std::vector<T> dense_;                     // packed values, no gaps
    std::vector<std::uint32_t> dense_to_slot_; // parallel to dense_: owning slot of each value
};

} // namespace rime::core
