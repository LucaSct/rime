// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/editorhost/editor_host.hpp"

#include <span>
#include <string_view>
#include <utility>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/core/reflect/serialize.hpp"
#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/world.hpp"

// The editor host's implementation. serialize_world / deserialize_world are the reflection walk:
// for every entity, for every *reflected* component, serialize its bytes through its TypeInfo.
// Nothing here names a concrete component type — a registered component is snapshot and edited for
// free (ADR-0018 §4 / ADR-0031). The EditorHost wraps that over a ProtocolConnection.
namespace rime::editorhost {
namespace {

// A snapshot/schema blob starts with this tag so a truncated or wrong-shaped buffer is rejected at
// the first read, not mis-parsed. (Bumped only if the editor blob layout changes.)
constexpr std::uint32_t kSnapshotMagic = 0x52534E31u; // 'R''S''N''1' — Rime SNapshot v1
constexpr std::uint32_t kSchemaMagic = 0x52534D31u;   // 'R''S''M''1' — Rime scheMa v1

// Resolve a component's stable type_hash to *this* world's registration-order ComponentId, or
// kInvalidComponentId if no reflected component with that hash is registered. Keying the wire on
// the hash (not the id) is what lets a blob move between two worlds whose registration order
// differs.
ecs::ComponentId id_for_type_hash(const ecs::ComponentRegistry& registry, std::uint64_t hash) {
    for (std::size_t i = 0; i < registry.count(); ++i) {
        const auto id = static_cast<ecs::ComponentId>(i);
        const ecs::ComponentInfo& info = registry.info(id);
        if (info.type_info != nullptr && info.type_info->type_hash == hash) {
            return id;
        }
    }
    return ecs::kInvalidComponentId;
}

} // namespace

bool is_editor_message(stream::MessageType type) noexcept {
    const auto v = static_cast<std::uint16_t>(type);
    return v >= static_cast<std::uint16_t>(stream::MessageType::EditorReservedBegin) &&
           v <= static_cast<std::uint16_t>(stream::MessageType::EditorReservedEnd);
}

std::vector<std::byte> serialize_world(const ecs::World& world) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u32(kSnapshotMagic);
    w.u32(static_cast<std::uint32_t>(world.entity_count()));

    const ecs::ComponentRegistry& registry = world.components();
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        const std::vector<ecs::ComponentId>& ids = arch.signature().ids();
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t row = 0; row < chunk.size(); ++row) {
                const ecs::Entity e = chunk.entity_at(row);
                // Entity identity: raw (index, generation), so an edit can name this exact entity.
                w.u32(e.index);
                w.u32(e.generation);
                // Only reflected components carry inspectable state; count and write those.
                std::uint16_t comp_count = 0;
                for (const ecs::ComponentId id : ids) {
                    if (registry.info(id).type_info != nullptr) {
                        ++comp_count;
                    }
                }
                w.u16(comp_count);
                for (const ecs::ComponentId id : ids) {
                    const ecs::ComponentInfo& info = registry.info(id);
                    if (info.type_info == nullptr) {
                        continue;
                    }
                    const std::vector<std::byte> blob =
                        core::serialize(*info.type_info, chunk.component(id, row));
                    w.u64(info.type_info->type_hash);
                    w.u32(static_cast<std::uint32_t>(blob.size()));
                    w.bytes(blob);
                }
            }
        }
    }
    return out;
}

bool deserialize_world(ecs::World& dst, std::span<const std::byte> data) {
    core::ByteReader r(data);
    std::uint32_t magic = 0;
    std::uint32_t entity_count = 0;
    if (!r.u32(magic) || magic != kSnapshotMagic || !r.u32(entity_count)) {
        RIME_ERROR("editorhost: bad snapshot header");
        return false;
    }
    const ecs::ComponentRegistry& registry = dst.components();
    for (std::uint32_t i = 0; i < entity_count; ++i) {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
        std::uint16_t comp_count = 0;
        if (!r.u32(index) || !r.u32(generation) || !r.u16(comp_count)) {
            RIME_ERROR("editorhost: truncated snapshot entity {}", i);
            return false;
        }
        (void)index;      // a reconstructed world gets fresh entity ids; the source ids are only
        (void)generation; // meaningful in a *live* session (the edit path), not on reload.
        const ecs::Entity e = dst.spawn();
        for (std::uint16_t c = 0; c < comp_count; ++c) {
            std::uint64_t hash = 0;
            std::uint32_t blob_len = 0;
            std::span<const std::byte> blob;
            if (!r.u64(hash) || !r.u32(blob_len) || !r.bytes(blob, blob_len)) {
                RIME_ERROR("editorhost: truncated snapshot component");
                return false;
            }
            const ecs::ComponentId id = id_for_type_hash(registry, hash);
            if (id == ecs::kInvalidComponentId) {
                RIME_ERROR("editorhost: snapshot has unknown component type_hash {:#x}", hash);
                return false;
            }
            void* slot = dst.add_component_raw(e, id);
            if (slot == nullptr || !core::deserialize(*registry.info(id).type_info, slot, blob)) {
                RIME_ERROR("editorhost: failed to apply snapshot component");
                return false;
            }
        }
    }
    return true;
}

std::vector<std::byte> serialize_schema(const ecs::World& world) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u32(kSchemaMagic);
    const ecs::ComponentRegistry& registry = world.components();
    std::uint32_t reflected = 0;
    for (std::size_t i = 0; i < registry.count(); ++i) {
        if (registry.info(static_cast<ecs::ComponentId>(i)).type_info != nullptr) {
            ++reflected;
        }
    }
    w.u32(reflected);
    for (std::size_t i = 0; i < registry.count(); ++i) {
        const ecs::ComponentInfo& info = registry.info(static_cast<ecs::ComponentId>(i));
        if (info.type_info == nullptr) {
            continue;
        }
        w.u64(info.type_info->type_hash);
        const std::string_view name = info.type_info->name != nullptr
                                          ? std::string_view(info.type_info->name)
                                          : std::string_view{};
        w.u16(static_cast<std::uint16_t>(name.size()));
        w.bytes(std::as_bytes(std::span(name.data(), name.size())));
    }
    return out;
}

bool apply_set_component(ecs::World& world,
                         ecs::Entity e,
                         std::uint64_t type_hash,
                         std::span<const std::byte> blob) {
    if (!world.is_alive(e)) {
        RIME_ERROR("editorhost: set-component on a dead entity");
        return false;
    }
    const ecs::ComponentRegistry& registry = world.components();
    const ecs::ComponentId id = id_for_type_hash(registry, type_hash);
    if (id == ecs::kInvalidComponentId) {
        RIME_ERROR("editorhost: set-component unknown type_hash {:#x}", type_hash);
        return false;
    }
    void* slot = world.get_component_raw(e, id);
    if (slot == nullptr) {
        slot = world.add_component_raw(e, id); // the entity lacked it — add it (archetype move)
    }
    if (slot == nullptr || !core::deserialize(*registry.info(id).type_info, slot, blob)) {
        return false;
    }
    world.mark_changed_raw(e, id);
    return true;
}

// ── EditorHost ──────────────────────────────────────────────────────────────────────────

EditorHost::EditorHost(stream::ProtocolConnection conn) noexcept : conn_(std::move(conn)) {}

bool EditorHost::send_hello(const ecs::World& world) {
    const std::vector<std::byte> schema = serialize_schema(world);
    const std::vector<std::byte> snapshot = serialize_world(world);
    return conn_.send_message(static_cast<stream::MessageType>(EditorMessage::Schema), schema) &&
           conn_.send_message(static_cast<stream::MessageType>(EditorMessage::Snapshot), snapshot);
}

bool EditorHost::poll_one(ecs::World& world) {
    stream::MessageType type{};
    std::vector<std::byte> payload;
    if (!conn_.recv_message(type, payload)) {
        return false; // connection closed / I/O error
    }
    if (type == stream::MessageType::Bye) {
        return false;
    }
    switch (static_cast<EditorMessage>(type)) {
        case EditorMessage::SetComponent: {
            core::ByteReader r(payload);
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            std::uint64_t hash = 0;
            std::uint32_t blob_len = 0;
            std::span<const std::byte> blob;
            if (!r.u32(index) || !r.u32(generation) || !r.u64(hash) || !r.u32(blob_len) ||
                !r.bytes(blob, blob_len)) {
                RIME_ERROR("editorhost: malformed SetComponent");
                return true; // ignore this message; stay connected
            }
            (void)apply_set_component(world, ecs::Entity{index, generation}, hash, blob);
            return true;
        }
        case EditorMessage::Spawn:
            (void)world.spawn();
            return true;
        case EditorMessage::Despawn: {
            core::ByteReader r(payload);
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            if (r.u32(index) && r.u32(generation)) {
                (void)world.despawn(ecs::Entity{index, generation});
            }
            return true;
        }
        default:
            return true; // an engine->editor or unknown type — nothing to apply
    }
}

} // namespace rime::editorhost
