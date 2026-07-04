// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/archetype.hpp"

#include <utility>

#include "rime/core/diagnostics/assert.hpp"

namespace rime::ecs {

Archetype::Archetype(ComponentSignature signature, ChunkLayout layout, ChunkPool& pool)
    : signature_(std::move(signature)), layout_(std::move(layout)), pool_(&pool) {}

Archetype::~Archetype() {
    // Destroy any live components (via ~Chunk) and return every buffer to the pool. Reset the Chunk
    // before freeing its buffer so destructors run while the storage is still valid.
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        chunks_[i].reset();
        pool_->deallocate(buffers_[i]);
    }
}

void Archetype::add_chunk() {
    void* buffer = pool_->allocate();
    buffers_.push_back(buffer);
    chunks_.push_back(std::make_unique<Chunk>(buffer, layout_));
}

Archetype::Row Archetype::insert(Entity entity) {
    if (chunks_.empty() || chunks_.back()->full()) {
        add_chunk();
    }
    const auto chunk_idx = static_cast<std::uint32_t>(chunks_.size() - 1);
    const std::uint32_t row = chunks_.back()->push_back(entity);
    ++count_;
    return Row{chunk_idx, row};
}

Archetype::Row Archetype::insert_uninitialized(Entity entity) {
    if (chunks_.empty() || chunks_.back()->full()) {
        add_chunk();
    }
    const auto chunk_idx = static_cast<std::uint32_t>(chunks_.size() - 1);
    const std::uint32_t row = chunks_.back()->push_back_uninitialized(entity);
    ++count_;
    return Row{chunk_idx, row};
}

void* Archetype::component(Row row, ComponentId id) noexcept {
    return chunks_[row.chunk]->component(id, row.row);
}

const void* Archetype::component(Row row, ComponentId id) const noexcept {
    return chunks_[row.chunk]->component(id, row.row);
}

void Archetype::destroy_row(Row row) {
    Chunk& c = *chunks_[row.chunk];
    for (const ColumnLayout& col : layout_.columns) {
        col.ops.destroy(c.component(col.id, row.row), 1);
    }
}

Entity Archetype::fill_hole(Row row) {
    RIME_ASSERT(!chunks_.empty());
    const auto last_chunk = static_cast<std::uint32_t>(chunks_.size() - 1);
    Chunk& last = *chunks_[last_chunk];
    RIME_ASSERT(!last.empty());
    const std::uint32_t last_row = last.size() - 1;

    Entity moved = kNullEntity;
    if (!(row.chunk == last_chunk && row.row == last_row)) {
        // Move the global-last element into the raw hole, then drop the (now moved-from) last row.
        // When row.chunk == last_chunk this is an in-chunk swap; otherwise it is cross-chunk.
        Chunk& target = *chunks_[row.chunk];
        const Entity moved_entity = last.entity_at(last_row);
        for (const ColumnLayout& col : layout_.columns) {
            void* dst = target.component(col.id, row.row); // raw hole
            void* src = last.component(col.id, last_row);  // live global-last
            col.ops.relocate(dst, src, 1);                 // move-construct into hole, destroy src
        }
        target.entities()[row.row] = moved_entity;
        moved = moved_entity;
    }
    last.pop_back_raw();
    --count_;

    // Keep the invariant that only the last chunk may be non-full by reclaiming an emptied trailing
    // chunk; an empty archetype then owns no chunks at all.
    if (last.empty()) {
        chunks_[last_chunk].reset();
        pool_->deallocate(buffers_[last_chunk]);
        buffers_.pop_back();
        chunks_.pop_back();
    }
    return moved;
}

Entity Archetype::remove(Row row) {
    destroy_row(row);
    return fill_hole(row);
}

} // namespace rime::ecs
