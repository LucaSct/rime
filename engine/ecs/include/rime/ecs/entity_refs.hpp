// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstring>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/reflect.hpp" // reflects Entity — the detector below keys on its TypeInfo

// **Entity references, found through reflection.** A component that points at another entity (a
// `Parent`, a weapon's owner, a door's linked switch) stores an `ecs::Entity` handle, and a handle
// is meaningful only inside the World that minted it. Every path that moves component data across
// an identity boundary — a `.rscene` save/load (handles ↔ scene-local ids), the editor wire
// (handles ↔ EditorIds), the play/stop restore (old handles ↔ new handles) — must rewrite those
// fields and ONLY those fields.
//
// The technique: the reflection system describes `Entity` itself (rime/ecs/reflect.hpp), so a
// field "is an entity reference" exactly when it is a Struct field whose nested TypeInfo IS
// Entity's. That makes the remap generic — a newly reflected component with an Entity field is
// remapped by every path above with zero code — and it is precise: we never scan a component's raw
// bytes for something that looks like a handle (a float or an asset id can hold any bit pattern).
//
// This header is the one copy of that rule. It started life private to the scene format (m9.2); the
// editor's identity layer (ADR-0075) is its second user, so it moved here rather than being copied.
namespace rime::ecs {

// True if `f` is an entity-reference field. Pointer identity is exact: `core::reflect<Entity>()`
// returns one function-local-static TypeInfo, and `make_field<Entity>` stamped that very address
// into the field's struct_type.
[[nodiscard]] inline bool is_entity_field(const core::Field& f) noexcept {
    return f.type == core::FieldType::Struct && f.struct_type == &core::reflect<Entity>();
}

// True if `type`, or any struct nested inside it, has an entity-reference field. Lets a caller skip
// the remap work entirely for the (common) component that cannot point at anything.
[[nodiscard]] inline bool has_entity_refs(const core::TypeInfo& type) noexcept {
    for (const core::Field& f : type.fields) {
        if (is_entity_field(f)) {
            return true;
        }
        if (f.type == core::FieldType::Struct && f.struct_type != nullptr &&
            has_entity_refs(*f.struct_type)) {
            return true;
        }
    }
    return false;
}

// Visit every entity-reference field of the `type` object at `base`, recursing through nested
// structs, and call `fn(Entity&)` on each — the callback may rewrite the handle in place. `fn`
// returns bool; a `false` stops the walk and is returned, so a remap can refuse on the first
// reference it cannot resolve. memcpy in and out because the object is raw component memory whose
// Entity field we only know by offset.
template <class F> bool for_each_entity_ref(const core::TypeInfo& type, std::byte* base, F&& fn) {
    for (const core::Field& f : type.fields) {
        if (f.type != core::FieldType::Struct || f.struct_type == nullptr) {
            continue;
        }
        std::byte* p = base + f.offset;
        if (is_entity_field(f)) {
            Entity e;
            std::memcpy(&e, p, sizeof(e));
            if (!fn(e)) {
                return false;
            }
            std::memcpy(p, &e, sizeof(e));
        } else if (!for_each_entity_ref(*f.struct_type, p, fn)) {
            return false;
        }
    }
    return true;
}

} // namespace rime::ecs
