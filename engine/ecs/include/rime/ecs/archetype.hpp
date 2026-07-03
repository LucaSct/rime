// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "rime/ecs/chunk.hpp"
#include "rime/ecs/chunk_pool.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/signature.hpp"

// An Archetype groups every entity whose components are exactly one ComponentSignature, and stores
// their data column-wise across a list of Chunks drawn from the ChunkPool (ADR-0018). It maintains
// the invariant that **every chunk but the last is full**, so an entity's position is a compact
// (chunk, row) pair and iteration is dense.
//
// Removal is the interesting part. An entity's row is vacated and then filled by the archetype's
// **global-last** element (the last row of the last chunk), which keeps the storage gap-free and is
// the cross-chunk generalization of a chunk swap-remove. Two callers need slightly different
// things, so removal is split:
//   * `destroy_row` runs the row's component destructors (used by despawn and by dropping a
//   component);
//   * `fill_hole` assumes the row's components are already gone (raw) — because they were either
//     destroyed or *moved out* to another archetype — and just relocates the global-last into it.
// An archetype move (add/remove component) moves the shared components out, then calls `fill_hole`;
// a despawn calls `remove` (= destroy_row + fill_hole). Both report the entity relocated into the
// hole so the World can fix that entity's directory location.
namespace rime::ecs {

class Archetype {
public:
    // An entity's position within this archetype.
    struct Row {
        std::uint32_t chunk;
        std::uint32_t row;
    };

    // Takes a precomputed layout (all chunks share it) and the pool chunks are drawn from.
    Archetype(ComponentSignature signature, ChunkLayout layout, ChunkPool& pool);
    ~Archetype();

    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;

    [[nodiscard]] const ComponentSignature& signature() const noexcept { return signature_; }

    [[nodiscard]] const ChunkLayout& layout() const noexcept { return layout_; }

    [[nodiscard]] std::size_t entity_count() const noexcept { return count_; }

    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

    // Append `entity`, default-constructing all its components. Returns its row.
    Row insert(Entity entity);

    // Append `entity` with its component storage left RAW — the caller must construct every
    // component (used by the archetype move, which relocates components in). Returns its row.
    Row insert_uninitialized(Entity entity);

    // Pointer to one of the row's components, or nullptr if `id` isn't in this archetype.
    [[nodiscard]] void* component(Row row, ComponentId id) noexcept;
    [[nodiscard]] const void* component(Row row, ComponentId id) const noexcept;

    // Run destructors on every component of `row` (the row stays occupied but raw). Pair with
    // fill_hole to complete a removal.
    void destroy_row(Row row);

    // Fill the (raw) hole at `row` with the archetype's global-last element and shrink; reclaims an
    // emptied trailing chunk. Returns the entity moved into `row`, or kNullEntity if `row` was the
    // global-last (nothing moved). Precondition: `row`'s components are already
    // destroyed/moved-out.
    Entity fill_hole(Row row);

    // Full removal for despawn: destroy_row + fill_hole. Returns the moved entity (see fill_hole).
    Entity remove(Row row);

    [[nodiscard]] Chunk& chunk(std::uint32_t index) noexcept { return *chunks_[index]; }

    [[nodiscard]] const Chunk& chunk(std::uint32_t index) const noexcept { return *chunks_[index]; }

private:
    void add_chunk();

    ComponentSignature signature_;
    ChunkLayout layout_;
    ChunkPool* pool_;
    std::vector<std::unique_ptr<Chunk>> chunks_;
    std::vector<void*> buffers_; // parallel to chunks_: the pooled buffer each Chunk wraps
    std::size_t count_ = 0;
};

} // namespace rime::ecs
