// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/ecs/chunk_pool.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/signature.hpp"

// A Chunk is a fixed-size block (from the ChunkPool) holding up to `capacity` entities' worth of
// components in **structure-of-arrays** form: one contiguous column per component type, plus a
// parallel Entity column naming the owner of each row (ADR-0018). Iterating a component across a
// chunk is therefore a tight linear scan — the whole reason archetype storage exists.
//
//   buffer:  [ Entity | Entity | … ][ pad ][ A | A | … ][ pad ][ B | B | … ] …
//            └── entity column ──┘         └─ column A ─┘       └─ column B ─┘
//
// The byte layout (capacity, per-column offsets) is the same for every chunk of an archetype, so it
// is computed once as a `ChunkLayout` and shared. Columns are packed alignment-first to minimize
// padding; lookup is by ComponentId regardless of physical order.
//
// A Chunk owns the *component objects* it constructs, not the buffer (the ChunkPool owns that).
// Removal is **swap-remove**: the last row is moved into the hole so the columns stay gap-free in
// O(component count), and the moved entity is reported so its directory location can be fixed up.
namespace rime::ecs {

// A monotonic CHANGE-DETECTION version (ADR-0018 §4). The World owns a global counter that advances
// each tick (per Schedule::run, or manually); every chunk stamps, per component column, the version
// at which that column was last written. A consumer asks "did column C change since version V?" and
// skips untouched chunks — the mechanism the editor's live inspector sync (M9), transform dirtying,
// and networked-destruction deltas (M11) all ride. 64-bit so the counter never wraps in practice
// (centuries at 60 Hz), which sidesteps the modular-comparison dance a 32-bit tick would need.
using Version = std::uint64_t;

// Returned by column lookups that find nothing.
inline constexpr std::uint32_t kNoColumn = 0xFFFFFFFFu;

// The byte offset, size, and lifecycle ops of one component column within a chunk.
struct ColumnLayout {
    ComponentId id;
    std::uint32_t offset; // byte offset of the column's base within the chunk buffer
    std::uint32_t size;   // sizeof(component)
    ComponentOps ops;     // default-construct / relocate / destroy
};

// The shared byte layout of every chunk in an archetype.
struct ChunkLayout {
    std::uint32_t capacity = 0;        // rows per chunk
    std::uint32_t entity_offset = 0;   // byte offset of the Entity column
    std::vector<ColumnLayout> columns; // one per component, sorted by ComponentId

    // The column for `id`, or nullptr if this layout has no such component.
    [[nodiscard]] const ColumnLayout* column(ComponentId id) const noexcept;

    // The index of `id`'s column within `columns` — the same index a chunk uses into its parallel
    // per-column version stamps — or kNoColumn if this layout has no such component.
    [[nodiscard]] std::uint32_t column_index(ComponentId id) const noexcept;
};

// Compute the SoA layout of a chunk for `sig`, taking each component's size/alignment/ops from
// `registry`. `capacity` is the most rows that fit in `chunk_size` bytes including the Entity
// column.
[[nodiscard]] ChunkLayout compute_chunk_layout(const ComponentSignature& sig,
                                               const ComponentRegistry& registry,
                                               std::size_t chunk_size = ChunkPool::kChunkSize);

class Chunk {
public:
    // Wrap a pooled `buffer` (kChunkSize bytes) as rows laid out by `layout`. `layout` must outlive
    // the Chunk (the archetype owns it). The Chunk starts empty.
    Chunk(void* buffer, const ChunkLayout& layout) noexcept;
    ~Chunk();

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = delete;
    Chunk& operator=(Chunk&&) = delete;

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return layout_->capacity; }

    [[nodiscard]] bool full() const noexcept { return size_ >= layout_->capacity; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Append a row owned by `entity`, default-constructing each component. Returns the new row
    // index. Precondition: !full().
    std::uint32_t push_back(Entity entity);

    // Append a row owned by `entity` but leave its component storage **raw** (uninitialized): the
    // caller must construct every component (by move or default) before the row is observed. This
    // is the source-side half of an archetype move (M4.2b), where components are moved in from
    // another archetype rather than default-constructed. Returns the new row index. Precondition:
    // !full().
    std::uint32_t push_back_uninitialized(Entity entity);

    // Drop the last row **without** destroying its components — the caller has already moved or
    // destroyed them (the archetype move relocates them out first). Precondition: !empty().
    void pop_back_raw() noexcept;

    // Remove `row` by moving the last row into its place (swap-remove). Returns the entity
    // relocated into `row` so the caller can update that entity's directory location — or
    // kNullEntity if `row` was the last row (nothing moved). Precondition: row < size().
    Entity swap_remove(std::uint32_t row);

    [[nodiscard]] Entity entity_at(std::uint32_t row) const noexcept;

    // Base pointer of a component column (nullptr if the component isn't in this chunk's
    // signature).
    [[nodiscard]] void* column(ComponentId id) noexcept;
    [[nodiscard]] const void* column(ComponentId id) const noexcept;

    // Pointer to one component within a row (nullptr if absent). Prefer the typed get<T>().
    [[nodiscard]] void* component(ComponentId id, std::uint32_t row) noexcept;
    [[nodiscard]] const void* component(ComponentId id, std::uint32_t row) const noexcept;

    template <class T> [[nodiscard]] T* get(ComponentId id, std::uint32_t row) noexcept {
        return static_cast<T*>(component(id, row));
    }

    // The Entity column (one entry per row) — the owner of each row, for iteration.
    [[nodiscard]] Entity* entities() noexcept;
    [[nodiscard]] const Entity* entities() const noexcept;

    // ---- change detection (ADR-0018 §4) ----

    // The version at which component `id`'s column was last written on this chunk (0 = never, or
    // `id` isn't in this chunk). A consumer compares it against its own last-seen version to decide
    // whether to scan the chunk at all.
    [[nodiscard]] Version column_version(ComponentId id) const noexcept;

    // Stamp component `id`'s column as written at version `v` — the writer discipline the ADR
    // requires (a system that mutates a column records the world version on the chunks it touched).
    // A no-op if `id` isn't in this chunk. Only advances the stamp (a later write never un-marks an
    // earlier one within the same version).
    void mark_changed(ComponentId id, Version v) noexcept;

private:
    std::byte* buffer_;
    const ChunkLayout* layout_;
    std::uint32_t size_ = 0;
    // Parallel to layout_->columns: the version each column was last written at (0 = never). Kept
    // per chunk (not per row) — the ADR's change-detection grain is the chunk column.
    std::vector<Version> column_versions_;
};

} // namespace rime::ecs
