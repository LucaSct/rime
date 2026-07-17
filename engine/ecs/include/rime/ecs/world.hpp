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

template <class... Ts> class Query; // defined in query.hpp (iterates the archetypes below)

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

    // ---- change detection (ADR-0018 §4) ----

    // The current world change version. Writers stamp the columns they mutate with it; a consumer
    // remembers it and later asks a query for what changed since. Starts at 1, so changed_since(0)
    // matches everything ever written.
    [[nodiscard]] Version version() const noexcept { return change_version_; }

    // Advance the world version by one and return the new value — the tick boundary separating
    // changes a consumer has already seen from ones it hasn't. The Schedule calls this once per
    // run(); an app driving the world without a Schedule (a fixed physics tick, a test) calls it
    // once per tick before that tick's writes, so those writes stamp a version later than the
    // consumer's last checkpoint.
    Version advance_version() noexcept { return ++change_version_; }

    // Record that entity `e`'s component T was written at the current version — stamps T's column
    // on e's chunk (ADR-0018's writer discipline). A no-op if `e` is dead or lacks T. This is how a
    // system that mutates data through get<T>() reports the write to change detection; a
    // change-tracking iteration (Query::for_each_changed) is the query side that then skips
    // untouched chunks.
    template <class T> void mark_changed(Entity e) noexcept {
        if (!registry_.is_registered<T>()) {
            return;
        }
        mark_changed_raw(e, registry_.id_of<T>());
    }

    // ---- introspection (tests + later ECS layers) ----

    [[nodiscard]] const ComponentRegistry& components() const noexcept { return registry_; }

    [[nodiscard]] std::size_t archetype_count() const noexcept { return archetypes_.size(); }

    // Archetype by index (< archetype_count()). Used by queries (query.hpp) and, later, the
    // parallel system scheduler (M4.4) to enumerate storage.
    [[nodiscard]] Archetype& archetype(std::size_t i) noexcept { return *archetypes_[i]; }

    [[nodiscard]] const Archetype& archetype(std::size_t i) const noexcept {
        return *archetypes_[i];
    }

    // Build a query over all entities that have every component in Ts. Iterate it with
    // Query::for_each — see query.hpp. Defined out-of-line there (Query is incomplete here).
    template <class... Ts> [[nodiscard]] Query<Ts...> query();

    // The signature of the archetype `e` currently lives in (the empty signature if `e` isn't
    // alive).
    [[nodiscard]] const ComponentSignature& signature_of(Entity e) const;

    // ---- type-erased component access (the M9 editor host + reflection-driven tools) ----
    // The typed get<T> / add_component<T> / mark_changed<T> above are sugar over these. A tool that
    // works by ComponentId rather than a static type — the editor host (engine/editorhost) walking
    // the world through the ComponentRegistry + reflection to (de)serialize any component — needs
    // the raw forms directly, so they are public. Same semantics as the typed versions: get returns
    // nullptr if `e` lacks the component; add returns a pointer to default-constructed storage
    // (relocating the entity's archetype); remove returns false if absent; mark_changed stamps the
    // component's column at the current world version (the change-detection ADR-0018 §4 designed
    // for exactly this consumer).
    [[nodiscard]] void* add_component_raw(Entity e, ComponentId id);
    bool remove_component_raw(Entity e, ComponentId id);
    [[nodiscard]] void* get_component_raw(Entity e, ComponentId id) noexcept;
    [[nodiscard]] const void* get_component_raw(Entity e, ComponentId id) const noexcept;
    void mark_changed_raw(Entity e, ComponentId id) noexcept;

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

    EntityDirectory directory_;
    ComponentRegistry registry_;
    ChunkPool pool_;
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::unordered_map<ComponentSignature, std::uint32_t> archetype_index_;
    std::uint32_t empty_archetype_ = 0;
    Version change_version_ = 1; // ADR-0018 §4: monotonic; 0 is reserved for "before any change"
};

} // namespace rime::ecs
