// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "rime/core/diagnostics/assert.hpp"
#include "rime/core/reflect/type_info.hpp"

// Components are the DATA of the ECS: plain structs (Position, Velocity, …) that will be stored in
// tight per-type columns inside archetypes (M4.2). Before storage can hold a component type
// *generically*, it must be REGISTERED — turned into runtime information the type-erased storage
// can act on without knowing the concrete C++ type. That is what this file provides.
//
// Registering a type T yields a ComponentInfo carrying:
//   * a ComponentId — a small dense integer assigned in registration order, used to index columns
//     and build archetype signatures (M4.2);
//   * T's size and alignment;
//   * type-erased ComponentOps (default-construct / relocate / destroy), so a chunk can build and
//     move rows during an archetype move (M4.3) by calling through function pointers; and
//   * T's reflection TypeInfo when T is reflected (RIME_REFLECT) — its field layout — so a
//   component
//     described once here is *serializable now* (M1.7) and *editor-inspectable later* (M9), exactly
//     the "register once ⇒ serialize + inspect" bet of ADR-0018. Reflection is optional (tags and
//     throwaway structs need not be reflected), but it is captured whenever present.
//
// M4 components must be trivially relocatable (ADR-0018) so a chunk move is a memcpy. We enforce
// the proxy std::is_trivially_copyable_v<T> at registration, and require default-constructibility
// (the storage default-constructs a slot when a component is added without an explicit value).
namespace rime::ecs {

// A dense component-type id, assigned in registration order (0, 1, 2, …). Strongly typed so it is
// never confused with a plain integer or an Entity index.
enum class ComponentId : std::uint32_t {};

inline constexpr ComponentId kInvalidComponentId{0xFFFFFFFFu};

// Type-erased lifecycle operations for one component type, generated once per T at registration.
// The storage layer holds these and calls them on memory it otherwise treats as raw bytes. `count`
// lets a whole contiguous run (a chunk's worth) be handled in a single call.
struct ComponentOps {
    void (*default_construct)(void* dst) noexcept;                      // placement-new one T()
    void (*relocate)(void* dst, void* src, std::size_t count) noexcept; // move src→dst, end sources
    void (*destroy)(void* ptr, std::size_t count) noexcept; // run dtors (no-op if trivial)
};

// The runtime description of a registered component type.
struct ComponentInfo {
    ComponentId id = kInvalidComponentId;
    const char* name = nullptr;  // reflected type name, or a fallback
    std::uint32_t size = 0;      // sizeof(T)
    std::uint32_t alignment = 0; // alignof(T)
    ComponentOps ops{};
    const core::TypeInfo* type_info = nullptr; // reflection fields, or null if T isn't reflected
};

namespace detail {

// An RTTI-free per-type identity key. The address of this function-local static is unique per T and
// stable for the whole run, so we key the type→id map on it (as a const void*) without needing
// <typeinfo> / RTTI — the classic "type id by static address" trick.
using TypeKey = const void*;

template <class T> [[nodiscard]] TypeKey type_key() noexcept {
    static const char key = 0;
    return &key;
}

// Build the ComponentOps for a concrete T. Trivially-copyable T (all M4 components) relocate by
// memcpy and need no destructor; the general branches are kept correct for the day non-trivial
// components are allowed, so the storage code above never has to change.
template <class T> [[nodiscard]] ComponentOps make_ops() noexcept {
    ComponentOps ops;
    ops.default_construct = [](void* dst) noexcept { ::new (dst) T(); };
    ops.relocate = [](void* dst, void* src, std::size_t count) noexcept {
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(dst, src, count * sizeof(T));
        } else {
            T* d = static_cast<T*>(dst);
            T* s = static_cast<T*>(src);
            for (std::size_t i = 0; i < count; ++i) {
                ::new (d + i) T(std::move(s[i]));
                s[i].~T();
            }
        }
    };
    ops.destroy = [](void* ptr, std::size_t count) noexcept {
        if constexpr (std::is_trivially_destructible_v<T>) {
            (void)ptr;
            (void)count; // nothing to do; trivially-destructible components leave no work
        } else {
            T* p = static_cast<T*>(ptr);
            for (std::size_t i = 0; i < count; ++i) {
                p[i].~T();
            }
        }
    };
    return ops;
}

} // namespace detail

// The component registry: assigns and remembers the ComponentInfo for each registered type. It is
// owned by the World (world.hpp) but defined standalone here so it is directly unit-testable and
// usable by the archetype layer.
class ComponentRegistry {
public:
    // Register T (idempotent): assign it a ComponentId the first time it is seen and capture its
    // info; return its id. Safe to call repeatedly — later calls just return the existing id.
    template <class T> ComponentId register_component() {
        static_assert(
            std::is_trivially_copyable_v<T>,
            "M4 components must be trivially copyable (ADR-0018: a chunk move is a memcpy)");
        static_assert(
            std::is_default_constructible_v<T>,
            "components must be default-constructible (storage default-constructs a slot)");
        const detail::TypeKey key = detail::type_key<T>();
        if (const auto it = index_of_.find(key); it != index_of_.end()) {
            return infos_[it->second].id;
        }
        const auto id = static_cast<ComponentId>(infos_.size());
        ComponentInfo info;
        info.id = id;
        info.size = static_cast<std::uint32_t>(sizeof(T));
        info.alignment = static_cast<std::uint32_t>(alignof(T));
        info.ops = detail::make_ops<T>();
        if constexpr (core::is_reflected_v<T>) {
            info.type_info = &core::reflect<T>();
            info.name = info.type_info->name; // reflected types carry their source name
        } else {
            info.name = "<unreflected component>";
        }
        index_of_.emplace(key, infos_.size());
        infos_.push_back(info);
        return id;
    }

    // True iff T has been registered.
    template <class T> [[nodiscard]] bool is_registered() const noexcept {
        return index_of_.find(detail::type_key<T>()) != index_of_.end();
    }

    // The ComponentId for an already-registered T. Asserts (checked builds) if T was never
    // registered — callers that might not have registered yet should go through
    // World::register_component<T>().
    template <class T> [[nodiscard]] ComponentId id_of() const {
        const auto it = index_of_.find(detail::type_key<T>());
        RIME_ASSERT_MSG(it != index_of_.end(), "component type not registered");
        return infos_[it->second].id;
    }

    // The full info for a component id.
    [[nodiscard]] const ComponentInfo& info(ComponentId id) const {
        const auto i = static_cast<std::size_t>(id);
        RIME_ASSERT_MSG(i < infos_.size(), "invalid ComponentId");
        return infos_[i];
    }

    // Number of registered component types.
    [[nodiscard]] std::size_t count() const noexcept { return infos_.size(); }

private:
    std::vector<ComponentInfo> infos_;                          // indexed by ComponentId
    std::unordered_map<detail::TypeKey, std::size_t> index_of_; // type identity → index into infos_
};

} // namespace rime::ecs
