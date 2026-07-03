// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/entity_directory.hpp"

// The World is the ECS front door: it owns the entity directory and the component registry, and —
// as M4 grows — the archetype storage (M4.2), queries (M4.3), and systems (M4.4). In M4.1 it
// provides the two foundations everything else stands on: the entity lifecycle and component-type
// registration.
//
// Threading contract (matching core::JobSystem): construct and *structurally* mutate the World from
// a single main thread — spawn/despawn/register are main-thread operations. Parallel systems (M4.4)
// may read and write component DATA across cores, but never restructure the world concurrently.
namespace rime::ecs {

class World {
public:
    // ---- entities ----

    // Create a new, empty entity (no components yet — those attach at M4.3).
    [[nodiscard]] Entity spawn() { return entities_.allocate(); }

    // Destroy an entity; returns false if it was already despawned/invalid.
    bool despawn(Entity e) noexcept { return entities_.free(e); }

    [[nodiscard]] bool is_alive(Entity e) const noexcept { return entities_.is_alive(e); }

    [[nodiscard]] std::size_t entity_count() const noexcept { return entities_.alive_count(); }

    // Hint the expected peak entity count so spawning avoids reallocations.
    void reserve_entities(std::size_t n) { entities_.reserve(n); }

    // ---- component types ----

    // Register T (idempotent) and return its ComponentId. This is the front door: a component type
    // is registered once, after which it has a stable id and — if reflected — is
    // serializable/inspectable.
    template <class T> ComponentId register_component() {
        return registry_.register_component<T>();
    }

    // The ComponentId of an already-registered T (asserts if not registered — see
    // register_component).
    template <class T> [[nodiscard]] ComponentId component_id() const {
        return registry_.id_of<T>();
    }

    template <class T> [[nodiscard]] bool is_registered() const noexcept {
        return registry_.is_registered<T>();
    }

    [[nodiscard]] std::size_t registered_component_count() const noexcept {
        return registry_.count();
    }

    // ---- subsystem access (used by later ECS layers, and by tests) ----
    [[nodiscard]] const ComponentRegistry& components() const noexcept { return registry_; }

    [[nodiscard]] EntityDirectory& entities() noexcept { return entities_; }

    [[nodiscard]] const EntityDirectory& entities() const noexcept { return entities_; }

private:
    EntityDirectory entities_;
    ComponentRegistry registry_;
};

} // namespace rime::ecs
