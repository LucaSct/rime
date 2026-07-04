// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/chunk.hpp"

#include <algorithm>

#include "rime/core/diagnostics/assert.hpp"
#include "rime/core/memory/allocator.hpp" // align_up

namespace rime::ecs {

namespace {
[[nodiscard]] std::uint32_t value_of(ComponentId id) noexcept {
    return static_cast<std::uint32_t>(id);
}
} // namespace

const ColumnLayout* ChunkLayout::column(ComponentId id) const noexcept {
    // A handful of columns — a linear scan is faster than anything cleverer and stays
    // cache-friendly.
    for (const ColumnLayout& col : columns) {
        if (col.id == id) {
            return &col;
        }
    }
    return nullptr;
}

ChunkLayout compute_chunk_layout(const ComponentSignature& sig,
                                 const ComponentRegistry& registry,
                                 std::size_t chunk_size) {
    // Gather each component's footprint from the registry.
    struct Col {
        ComponentId id;
        std::uint32_t size;
        std::uint32_t align;
        ComponentOps ops;
    };

    std::vector<Col> cols;
    cols.reserve(sig.size());
    std::size_t sum_sizes = 0;
    for (const ComponentId id : sig.ids()) {
        const ComponentInfo& info = registry.info(id);
        cols.push_back(Col{id, info.size, info.alignment, info.ops});
        sum_sizes += info.size;
    }

    const std::size_t entity_size = sizeof(Entity);
    const std::size_t row_bytes = entity_size + sum_sizes; // packed, ignoring inter-column padding
    RIME_ASSERT_MSG(row_bytes <= chunk_size, "a single ECS row exceeds the chunk size");

    // Lay columns out in DESCENDING alignment order to minimize padding between them. Physical
    // order is free to choose because lookup is by id (column() scans), so we optimize packing
    // here.
    std::sort(
        cols.begin(), cols.end(), [](const Col& a, const Col& b) { return a.align > b.align; });

    // Byte offset just past the last column for a given row capacity (Entity column first at offset
    // 0).
    const auto layout_end = [&](std::uint32_t cap) -> std::size_t {
        std::size_t off = entity_size * cap;
        for (const Col& c : cols) {
            off = core::align_up(off, c.align);
            off += static_cast<std::size_t>(c.size) * cap;
        }
        return off;
    };

    // Start from the padding-free estimate and shrink until the aligned layout fits. Padding is
    // bounded by the sum of alignments (a small constant), so this trims at most a few rows.
    std::uint32_t capacity = static_cast<std::uint32_t>(chunk_size / row_bytes);
    while (capacity > 1 && layout_end(capacity) > chunk_size) {
        --capacity;
    }
    RIME_ASSERT(capacity >= 1);
    RIME_ASSERT_MSG(layout_end(capacity) <= chunk_size,
                    "chunk layout overflow (components too large)");

    ChunkLayout layout;
    layout.capacity = capacity;
    layout.entity_offset = 0;
    layout.columns.reserve(cols.size());
    std::size_t off = entity_size * capacity;
    for (const Col& c : cols) {
        off = core::align_up(off, c.align);
        layout.columns.push_back(
            ColumnLayout{c.id, static_cast<std::uint32_t>(off), c.size, c.ops});
        off += static_cast<std::size_t>(c.size) * capacity;
    }
    // Store columns sorted by id so lookups and iteration are order-stable (independent of the
    // alignment-based physical packing above).
    std::sort(layout.columns.begin(),
              layout.columns.end(),
              [](const ColumnLayout& a, const ColumnLayout& b) {
                  return value_of(a.id) < value_of(b.id);
              });
    return layout;
}

Chunk::Chunk(void* buffer, const ChunkLayout& layout) noexcept
    : buffer_(static_cast<std::byte*>(buffer)), layout_(&layout) {}

Chunk::~Chunk() {
    // Destroy any still-live component objects (a no-op for M4's trivially-destructible components,
    // but correct in general). The Entity column is trivial; the buffer is freed by the ChunkPool.
    for (const ColumnLayout& col : layout_->columns) {
        col.ops.destroy(buffer_ + col.offset, size_);
    }
}

Entity* Chunk::entities() noexcept {
    return reinterpret_cast<Entity*>(buffer_ + layout_->entity_offset);
}

const Entity* Chunk::entities() const noexcept {
    return reinterpret_cast<const Entity*>(buffer_ + layout_->entity_offset);
}

Entity Chunk::entity_at(std::uint32_t row) const noexcept {
    RIME_ASSERT(row < size_);
    return entities()[row];
}

void* Chunk::column(ComponentId id) noexcept {
    const ColumnLayout* c = layout_->column(id);
    return c ? buffer_ + c->offset : nullptr;
}

const void* Chunk::column(ComponentId id) const noexcept {
    const ColumnLayout* c = layout_->column(id);
    return c ? buffer_ + c->offset : nullptr;
}

void* Chunk::component(ComponentId id, std::uint32_t row) noexcept {
    const ColumnLayout* c = layout_->column(id);
    if (c == nullptr) {
        return nullptr;
    }
    RIME_ASSERT(row < size_);
    return buffer_ + c->offset + static_cast<std::size_t>(row) * c->size;
}

const void* Chunk::component(ComponentId id, std::uint32_t row) const noexcept {
    const ColumnLayout* c = layout_->column(id);
    if (c == nullptr) {
        return nullptr;
    }
    RIME_ASSERT(row < size_);
    return buffer_ + c->offset + static_cast<std::size_t>(row) * c->size;
}

std::uint32_t Chunk::push_back(Entity entity) {
    RIME_ASSERT(!full());
    const std::uint32_t row = size_;
    entities()[row] = entity;
    for (const ColumnLayout& col : layout_->columns) {
        col.ops.default_construct(buffer_ + col.offset + static_cast<std::size_t>(row) * col.size);
    }
    ++size_;
    return row;
}

std::uint32_t Chunk::push_back_uninitialized(Entity entity) {
    RIME_ASSERT(!full());
    const std::uint32_t row = size_;
    entities()[row] = entity; // Entity is trivial; the component columns stay raw for the caller.
    ++size_;
    return row;
}

void Chunk::pop_back_raw() noexcept {
    RIME_ASSERT(!empty());
    --size_; // the caller already moved/destroyed the last row's components
}

Entity Chunk::swap_remove(std::uint32_t row) {
    RIME_ASSERT(row < size_);
    const std::uint32_t last = size_ - 1;
    for (const ColumnLayout& col : layout_->columns) {
        std::byte* base = buffer_ + col.offset;
        std::byte* dst = base + static_cast<std::size_t>(row) * col.size;
        col.ops.destroy(dst, 1); // end the removed component first
        if (row != last) {
            std::byte* src = base + static_cast<std::size_t>(last) * col.size;
            col.ops.relocate(dst, src, 1); // move the last row's component into the hole
        }
    }
    Entity moved = kNullEntity;
    if (row != last) {
        entities()[row] = entities()[last];
        moved = entities()[row]; // the entity that now lives at `row`
    }
    --size_;
    return moved;
}

} // namespace rime::ecs
