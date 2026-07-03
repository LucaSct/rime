// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M4.2a (chunk layout + SoA row store). The layout packs each component into its own
// aligned column; push_back appends a row and round-trips per-component values; swap_remove keeps
// the columns gap-free and reports the moved entity so a directory can be fixed up; the empty
// signature (entities with no components) still works. Memory correctness is also covered under
// ASan.

#include <doctest/doctest.h>

#include <cstdint>

#include "rime/ecs/chunk.hpp"
#include "rime/ecs/chunk_pool.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/signature.hpp"

using namespace rime::ecs;

namespace ck {
struct Pos {
    float x;
    float y;
    float z;
};

struct Vel {
    double dx; // double → 8-byte alignment, so the layout must align this column
    double dy;
};

struct Extra {
    std::uint32_t v;
};
} // namespace ck

namespace {
Entity ent(std::uint32_t i) noexcept {
    return Entity{i, 0};
}
} // namespace

TEST_CASE("layout gives aligned, non-overlapping columns with a sane capacity") {
    ComponentRegistry reg;
    const ComponentId pos = reg.register_component<ck::Pos>();
    const ComponentId vel = reg.register_component<ck::Vel>();

    const ChunkLayout layout = compute_chunk_layout(ComponentSignature{pos, vel}, reg);
    CHECK(layout.capacity >= 1);
    REQUIRE(layout.columns.size() == 2);

    const ColumnLayout* pc = layout.column(pos);
    const ColumnLayout* vc = layout.column(vel);
    REQUIRE(pc != nullptr);
    REQUIRE(vc != nullptr);
    CHECK(pc->size == sizeof(ck::Pos));
    CHECK(vc->size == sizeof(ck::Vel));
    CHECK(vc->offset % alignof(ck::Vel) == 0); // the double column is 8-byte aligned

    // Everything (Entity column + both component columns) fits inside one chunk.
    const std::size_t used = static_cast<std::size_t>(layout.capacity) *
                             (sizeof(Entity) + sizeof(ck::Pos) + sizeof(ck::Vel));
    CHECK(used <= ChunkPool::kChunkSize);
}

TEST_CASE("push_back appends rows and round-trips every component") {
    ComponentRegistry reg;
    const ComponentId pos = reg.register_component<ck::Pos>();
    const ComponentId vel = reg.register_component<ck::Vel>();
    const ChunkLayout layout = compute_chunk_layout(ComponentSignature{pos, vel}, reg);

    ChunkPool memory;
    void* buffer = memory.allocate();
    {
        Chunk chunk(buffer, layout);
        CHECK(chunk.empty());

        constexpr std::uint32_t kRows = 5;
        for (std::uint32_t i = 0; i < kRows; ++i) {
            const std::uint32_t row = chunk.push_back(ent(i));
            CHECK(row == i);
            *chunk.get<ck::Pos>(pos, row) =
                ck::Pos{static_cast<float>(i), i + 0.5f, -static_cast<float>(i)};
            *chunk.get<ck::Vel>(vel, row) = ck::Vel{i * 10.0, i * 100.0};
        }
        CHECK(chunk.size() == kRows);

        for (std::uint32_t i = 0; i < kRows; ++i) {
            CHECK(chunk.entity_at(i) == ent(i));
            const ck::Pos& p = *chunk.get<ck::Pos>(pos, i);
            const ck::Vel& v = *chunk.get<ck::Vel>(vel, i);
            CHECK(p.x == static_cast<float>(i));
            CHECK(p.y == i + 0.5f);
            CHECK(p.z == -static_cast<float>(i));
            CHECK(v.dx == i * 10.0);
            CHECK(v.dy == i * 100.0);
        }

        // A component that isn't in this chunk's signature resolves to nullptr.
        const ComponentId extra = reg.register_component<ck::Extra>();
        CHECK(chunk.column(extra) == nullptr);
    }
    memory.deallocate(buffer);
}

TEST_CASE("swap_remove keeps columns packed and reports the moved entity") {
    ComponentRegistry reg;
    const ComponentId pos = reg.register_component<ck::Pos>();
    const ChunkLayout layout = compute_chunk_layout(ComponentSignature{pos}, reg);

    ChunkPool memory;
    void* buffer = memory.allocate();
    {
        Chunk chunk(buffer, layout);
        for (std::uint32_t i = 0; i < 4; ++i) { // entities 0,1,2,3
            const std::uint32_t row = chunk.push_back(ent(i));
            chunk.get<ck::Pos>(pos, row)->x = static_cast<float>(i);
        }

        // Remove row 1 (entity 1). The last row (entity 3) swaps into slot 1 and is reported.
        const Entity moved = chunk.swap_remove(1);
        CHECK(moved == ent(3));
        CHECK(chunk.size() == 3);
        CHECK(chunk.entity_at(1) == ent(3));          // the mover landed here...
        CHECK(chunk.get<ck::Pos>(pos, 1)->x == 3.0f); // ...with its data intact
        CHECK(chunk.entity_at(0) == ent(0));          // untouched neighbour
        CHECK(chunk.get<ck::Pos>(pos, 0)->x == 0.0f);
        CHECK(chunk.entity_at(2) == ent(2));

        // Removing the last row moves nothing → kNullEntity.
        const Entity none = chunk.swap_remove(chunk.size() - 1);
        CHECK(none == kNullEntity);
        CHECK(chunk.size() == 2);
    }
    memory.deallocate(buffer);
}

TEST_CASE("a chunk fills to exactly its capacity") {
    ComponentRegistry reg;
    const ComponentId pos = reg.register_component<ck::Pos>();
    const ChunkLayout layout = compute_chunk_layout(ComponentSignature{pos}, reg);

    ChunkPool memory;
    void* buffer = memory.allocate();
    {
        Chunk chunk(buffer, layout);
        for (std::uint32_t i = 0; i < layout.capacity; ++i) {
            (void)chunk.push_back(ent(i));
        }
        CHECK(chunk.size() == layout.capacity);
        CHECK(chunk.full());
    }
    memory.deallocate(buffer);
}

TEST_CASE("the empty signature stores entities with no components") {
    ComponentRegistry reg;
    const ChunkLayout layout = compute_chunk_layout(ComponentSignature{}, reg);
    CHECK(layout.columns.empty());
    CHECK(layout.capacity == ChunkPool::kChunkSize / sizeof(Entity)); // only the Entity column

    ChunkPool memory;
    void* buffer = memory.allocate();
    {
        Chunk chunk(buffer, layout);
        const std::uint32_t r0 = chunk.push_back(ent(100));
        const std::uint32_t r1 = chunk.push_back(ent(200));
        CHECK(chunk.entity_at(r0) == ent(100));
        CHECK(chunk.entity_at(r1) == ent(200));

        const Entity moved = chunk.swap_remove(r0);
        CHECK(moved == ent(200)); // the last entity swapped down into r0
        CHECK(chunk.entity_at(r0) == ent(200));
        CHECK(chunk.size() == 1);
    }
    memory.deallocate(buffer);
}
