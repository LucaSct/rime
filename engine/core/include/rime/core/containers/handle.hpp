// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

// A Handle is a generational index: a stable, copyable "pointer" into a SlotMap that survives
// the container moving its storage around, and that *detects* use-after-free. It is two 32-bit
// fields — a slot `index` and a `generation` stamp. When a slot is freed and later reused, its
// generation is bumped, so any handle still holding the old generation is recognized as stale
// instead of silently aliasing the new occupant (the classic dangling-pointer bug). This is the
// backbone of data-oriented resource/entity references across the engine (ECS, assets, GPU
// objects): systems pass cheap 8-byte handles, never raw pointers into relocatable arrays.
namespace rime::core {

// Sentinel for "no slot". Real slot indices are < this, so a default-constructed Handle is
// invalid and never collides with a live slot.
inline constexpr std::uint32_t kInvalidSlotIndex = 0xFFFFFFFFu;

// Phantom-typed on T so a Handle<Mesh> cannot be passed where a Handle<Texture> is expected —
// the type system enforces that handles and their SlotMap agree, at zero runtime cost.
template <class T> struct Handle {
    std::uint32_t index = kInvalidSlotIndex;
    std::uint32_t generation = 0;

    // A handle is "null" purely structurally (bad index); whether it still refers to a *live*
    // element is a question only its SlotMap can answer (generation match) — see SlotMap::contains.
    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != kInvalidSlotIndex; }

    friend constexpr bool operator==(Handle a, Handle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }

    friend constexpr bool operator!=(Handle a, Handle b) noexcept { return !(a == b); }
};

} // namespace rime::core
