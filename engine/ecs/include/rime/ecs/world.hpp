// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk_pool.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/entity_directory.hpp"
#include "rime/ecs/signature.hpp"

// The World is the ECS front door and the owner of everything: the entity directory, the component
// registry, the chunk pool, and the archetypes. As of M4.2b it actually **holds components on
// entities**:
//   * spawn() places an entity in the empty archetype (signature {});
//   * add_component / remove_component relocate it between archetypes — the "archetype move": a row
//     is inserted in the destination, shared components are moved across, the new one is
//     constructed (or the dropped one destroyed), and the source row is vacated (fixing up
//     whichever entity is swapped into it);
//   * get<T> / has<T> resolve an entity's directory location → archetype → chunk → component.
// The entity directory's `location` field, reserved since M4.1, is now the live index into storage.
//
// Threading contract (matching core::JobSystem): construct and structurally mutate the World from a
// single main thread. Parallel systems (M4.4) read/write component data across cores but never
// restructure the world concurrently.
namespace rime::ecs {

class World {
public:
    World();
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---- entities ----

    // Create a new entity with no components (it lives in the empty archetype).
    [[nodiscard]] Entity spawn();

    // Spawn an entity already carrying `comps`, built by successive archetype moves. Ergonomic
    // sugar over spawn() + add_component; a direct-to-archetype fast path can come later if
    // profiling wants.
    template <class... Ts> [[nodiscard]] Entity spawn_with(const Ts&... comps) {
        const Entity e = spawn();
        (add_component<Ts>(e, comps), ...);
        return e;
    }

    // Destroy an entity and its components; returns false if it was already despawned/invalid.
    bool despawn(Entity e);

    [[nodiscard]] bool is_alive(Entity e) const noexcept { return directory_.is_alive(e); }

    [[nodiscard]] std::size_t entity_count() const noexcept { return directory_.alive_count(); }

    void reserve_entities(std::size_t n) { directory_.reserve(n); }

    // ---- components on entities ----

    // Add component T to `e` (or overwrite it if already present). Returns a pointer to the stored
    // component, or nullptr if `e` is not alive.
    template <class T> T* add_component(Entity e, const T& value) {
        void* slot = add_component_raw(e, register_component<T>());
        if (slot == nullptr) {
            return nullptr;
        }
        T* typed = static_cast<T*>(slot);
        *typed = value;
        return typed;
    }

    // Remove component T from `e`. Returns true iff it was present and removed.
    template <class T> bool remove_component(Entity e) {
        if (!registry_.is_registered<T>()) {
            return false; // a never-registered type cannot be on any entity
        }
        return remove_component_raw(e, registry_.id_of<T>());
    }

    // Pointer to `e`'s component T, or nullptr if `e` is not alive or lacks T.
    template <class T> [[nodiscard]] T* get(Entity e) noexcept {
        if (!registry_.is_registered<T>()) {
            return nullptr;
        }
        return static_cast<T*>(get_component_raw(e, registry_.id_of<T>()));
    }

    template <class T> [[nodiscard]] const T* get(Entity e) const noexcept {
        if (!registry_.is_registered<T>()) {
            return nullptr;
        }
        return static_cast<const T*>(get_component_raw(e, registry_.id_of<T>()));
    }

    template <class T> [[nodiscard]] bool has(Entity e) const noexcept {
        return get<T>(e) != nullptr;
    }

    // ---- component types ----

    template <class T> ComponentId register_component() {
        return registry_.register_component<T>();
    }

    template <class T> [[nodiscard]] ComponentId component_id() const {
        return registry_.id_of<T>();
    }

    template <class T> [[nodiscard]] bool is_registered() const noexcept {
        return registry_.is_registered<T>();
    }

    [[nodiscard]] std::size_t registered_component_count() const noexcept {
        return registry_.count();
    }

    // ---- introspection (tests + later ECS layers) ----

    [[nodiscard]] const ComponentRegistry& components() const noexcept { return registry_; }

    [[nodiscard]] std::size_t archetype_count() const noexcept { return archetypes_.size(); }

    // The signature of the archetype `e` currently lives in (the empty signature if `e` isn't
    // alive).
    [[nodiscard]] const ComponentSignature& signature_of(Entity e) const;

private:
    // Find the archetype for `sig`, creating it (and its chunk layout) on first use. Returns its
    // index. NB: may grow archetypes_, so callers must re-fetch archetype references by index
    // after.
    std::uint32_t get_or_create_archetype(const ComponentSignature& sig);

    // Move `e` from `src_idx` to `dst_idx`: relocate components present in both, destroy components
    // only in the source, leave components only in the destination RAW (the caller constructs
    // them). Fixes up the directory location of `e` and of any entity swapped into e's vacated
    // source row. Returns e's new row in the destination archetype.
    Archetype::Row
    relocate_entity(Entity e, EntityLocation src, std::uint32_t src_idx, std::uint32_t dst_idx);

    void* add_component_raw(Entity e, ComponentId id);
    bool remove_component_raw(Entity e, ComponentId id);
    [[nodiscard]] void* get_component_raw(Entity e, ComponentId id) noexcept;
    [[nodiscard]] const void* get_component_raw(Entity e, ComponentId id) const noexcept;

    EntityDirectory directory_;
    ComponentRegistry registry_;
    ChunkPool pool_;
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::unordered_map<ComponentSignature, std::uint32_t> archetype_index_;
    std::uint32_t empty_archetype_ = 0;
};

} // namespace rime::ecs
