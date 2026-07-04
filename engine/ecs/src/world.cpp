// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/world.hpp"

#include <utility>

namespace rime::ecs {

World::World() {
    // The empty archetype (signature {}) holds every freshly-spawned, component-less entity.
    // Creating it up front means spawn() never special-cases "no archetype yet."
    empty_archetype_ = get_or_create_archetype(ComponentSignature{});
}

World::~World() = default;

std::uint32_t World::get_or_create_archetype(const ComponentSignature& sig) {
    if (const auto it = archetype_index_.find(sig); it != archetype_index_.end()) {
        return it->second;
    }
    ChunkLayout layout = compute_chunk_layout(sig, registry_);
    const auto idx = static_cast<std::uint32_t>(archetypes_.size());
    archetypes_.push_back(std::make_unique<Archetype>(sig, std::move(layout), pool_));
    archetype_index_.emplace(sig, idx);
    return idx;
}

Entity World::spawn() {
    const Entity e = directory_.allocate();
    const Archetype::Row row = archetypes_[empty_archetype_]->insert(e);
    *directory_.location(e) = EntityLocation{empty_archetype_, row.chunk, row.row};
    return e;
}

bool World::despawn(Entity e) {
    const EntityLocation* locp = directory_.location(e);
    if (locp == nullptr) {
        return false;
    }
    const EntityLocation loc = *locp;
    const Entity moved = archetypes_[loc.archetype]->remove(Archetype::Row{loc.chunk, loc.row});
    if (moved != kNullEntity) {
        // The entity swapped into e's slot now lives at e's old (archetype, chunk, row).
        *directory_.location(moved) = loc;
    }
    directory_.free(e);
    return true;
}

Archetype::Row
World::relocate_entity(Entity e, EntityLocation src, std::uint32_t src_idx, std::uint32_t dst_idx) {
    Archetype& src_arch = *archetypes_[src_idx];
    Archetype& dst_arch = *archetypes_[dst_idx];
    const Archetype::Row src_row{src.chunk, src.row};
    const Archetype::Row dst_row = dst_arch.insert_uninitialized(e);

    // Move the components the two archetypes share; leave destination-only components RAW for the
    // caller (add_component constructs the new one; a remove has no destination-only components).
    for (const ColumnLayout& col : dst_arch.layout().columns) {
        if (src_arch.signature().contains(col.id)) {
            void* d = dst_arch.component(dst_row, col.id);
            void* s = src_arch.component(src_row, col.id);
            col.ops.relocate(d, s, 1);
        }
    }
    // Destroy components that exist only in the source (the one dropped by remove_component).
    for (const ColumnLayout& col : src_arch.layout().columns) {
        if (!dst_arch.signature().contains(col.id)) {
            col.ops.destroy(src_arch.component(src_row, col.id), 1);
        }
    }

    // The source row's components are now all moved-out or destroyed (raw): vacate it, and fix up
    // whichever entity gets swapped into the hole.
    const Entity moved = src_arch.fill_hole(src_row);
    if (moved != kNullEntity) {
        *directory_.location(moved) = src;
    }
    *directory_.location(e) = EntityLocation{dst_idx, dst_row.chunk, dst_row.row};
    return dst_row;
}

void* World::add_component_raw(Entity e, ComponentId id) {
    const EntityLocation* locp = directory_.location(e);
    if (locp == nullptr) {
        return nullptr; // not alive
    }
    const EntityLocation src = *locp;
    if (archetypes_[src.archetype]->signature().contains(id)) {
        // Already present — hand back the existing slot to overwrite in place (no move).
        return archetypes_[src.archetype]->component(Archetype::Row{src.chunk, src.row}, id);
    }
    // get_or_create_archetype may grow archetypes_, so compute the target signature first, then
    // relocate by index (relocate_entity re-fetches the archetypes).
    const ComponentSignature target = archetypes_[src.archetype]->signature().with(id);
    const std::uint32_t dst_idx = get_or_create_archetype(target);
    const Archetype::Row dst_row = relocate_entity(e, src, src.archetype, dst_idx);

    // The added component is destination-only, so relocate_entity left its storage RAW — construct
    // it.
    Archetype& dst_arch = *archetypes_[dst_idx];
    void* slot = dst_arch.component(dst_row, id);
    dst_arch.layout().column(id)->ops.default_construct(slot);
    return slot;
}

bool World::remove_component_raw(Entity e, ComponentId id) {
    const EntityLocation* locp = directory_.location(e);
    if (locp == nullptr) {
        return false;
    }
    const EntityLocation src = *locp;
    if (!archetypes_[src.archetype]->signature().contains(id)) {
        return false; // doesn't have it
    }
    const ComponentSignature target = archetypes_[src.archetype]->signature().without(id);
    const std::uint32_t dst_idx = get_or_create_archetype(target);
    relocate_entity(e, src, src.archetype, dst_idx); // the dropped component is destroyed inside
    return true;
}

void* World::get_component_raw(Entity e, ComponentId id) noexcept {
    const EntityLocation* locp = directory_.location(e);
    if (locp == nullptr) {
        return nullptr;
    }
    Archetype& a = *archetypes_[locp->archetype];
    if (!a.signature().contains(id)) {
        return nullptr;
    }
    return a.component(Archetype::Row{locp->chunk, locp->row}, id);
}

const void* World::get_component_raw(Entity e, ComponentId id) const noexcept {
    const EntityLocation* locp = directory_.location(e);
    if (locp == nullptr) {
        return nullptr;
    }
    const Archetype& a = *archetypes_[locp->archetype];
    if (!a.signature().contains(id)) {
        return nullptr;
    }
    return a.component(Archetype::Row{locp->chunk, locp->row}, id);
}

const ComponentSignature& World::signature_of(Entity e) const {
    static const ComponentSignature kEmpty;
    const EntityLocation* locp = directory_.location(e);
    if (locp == nullptr) {
        return kEmpty;
    }
    return archetypes_[locp->archetype]->signature();
}

} // namespace rime::ecs
