// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/wire_schema.hpp"

#include <algorithm>

#include "rime/core/reflect/serialize.hpp"
// For the RIME_REFLECT registration of ecs::Entity itself — this file needs its fingerprint to
// recognize entity-reference fields, which is a different need from merely naming the type.
#include "rime/ecs/reflect.hpp"

namespace rime::replication {
namespace {

// The fingerprint of `ecs::Entity` itself. Computed once, lazily, from the reflection registration
// in ecs/reflect.hpp — comparing hashes rather than TypeInfo addresses keeps this honest across
// translation units, where the same type can legitimately have been reflected through a different
// static.
[[nodiscard]] std::uint64_t entity_type_hash() {
    static const std::uint64_t hash = core::reflect<ecs::Entity>().type_hash;
    return hash;
}

// Depth-first search for an Entity-shaped field anywhere in the type.
[[nodiscard]] bool contains_entity_field(const core::TypeInfo& type) noexcept {
    if (type.type_hash == entity_type_hash()) {
        return true;
    }
    for (const core::Field& field : type.fields) {
        if (field.type == core::FieldType::Struct && field.struct_type != nullptr &&
            contains_entity_field(*field.struct_type)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool WireSchema::is_replicable(const core::TypeInfo& type) noexcept {
    return !contains_entity_field(type);
}

WireSchema WireSchema::build(const ecs::ComponentRegistry& registry) {
    WireSchema schema;
    schema.by_local_.assign(registry.count(), kInvalidWireComponentId);

    for (std::size_t i = 0; i < registry.count(); ++i) {
        const auto local = static_cast<ecs::ComponentId>(i);
        const ecs::ComponentInfo& info = registry.info(local);
        if (info.type_info == nullptr) {
            continue; // unreflected: a tag or a runtime-only handle, never serialized anywhere
        }
        if (!is_replicable(*info.type_info)) {
            schema.excluded_names_.emplace_back(info.type_info->name);
            continue;
        }
        schema.entries_.push_back(Entry{local, info.type_info, core::packed_size(*info.type_info)});
    }

    // THE ordering. Sorting by type_hash — not by name, not by ComponentId — is what makes the
    // table identical on two peers that registered the same components in different orders, which
    // is exactly the guarantee `ecs::component_schema_hash` relies on and the handshake has already
    // verified by the time anyone builds one of these.
    std::sort(schema.entries_.begin(), schema.entries_.end(), [](const Entry& a, const Entry& b) {
        return a.type->type_hash < b.type->type_hash;
    });

    for (std::size_t i = 0; i < schema.entries_.size(); ++i) {
        schema.by_local_[static_cast<std::size_t>(schema.entries_[i].local)] =
            static_cast<WireComponentId>(i);
    }

    // Excluded names are sorted for the same reason: a startup diagnostic that lists components in
    // registration order would read differently on two peers running the same build.
    std::sort(schema.excluded_names_.begin(), schema.excluded_names_.end());
    return schema;
}

WireComponentId WireSchema::wire_id_of(ecs::ComponentId local) const noexcept {
    const auto i = static_cast<std::size_t>(local);
    return i < by_local_.size() ? by_local_[i] : kInvalidWireComponentId;
}

bool WireSchema::lookup(WireComponentId wire,
                        ecs::ComponentId& local_out,
                        const core::TypeInfo*& type_out,
                        std::size_t& packed_size_out) const noexcept {
    const auto i = static_cast<std::size_t>(wire);
    if (i >= entries_.size()) {
        return false;
    }
    const Entry& entry = entries_[i];
    local_out = entry.local;
    type_out = entry.type;
    packed_size_out = entry.packed_size;
    return true;
}

} // namespace rime::replication
