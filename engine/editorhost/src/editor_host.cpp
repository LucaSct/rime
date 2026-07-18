// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/editorhost/editor_host.hpp"

#include <span>
#include <string_view>
#include <unordered_set>
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
// v2 (m9.4): the schema now carries each type's full field layout, not just its name, so the editor
// can *generate* typed inspectors from it. The tag bump makes an old editor reject the new shape.
constexpr std::uint32_t kSchemaMagic = 0x52534D32u; // 'R''S''M''2' — Rime scheMa v2

// Write a length-prefixed name ([len:u16][utf8 bytes]); a null name writes an empty string.
void write_name(core::ByteWriter& w, const char* name) {
    const std::string_view s = name != nullptr ? std::string_view(name) : std::string_view{};
    w.u16(static_cast<std::uint16_t>(s.size()));
    w.bytes(std::as_bytes(std::span(s.data(), s.size())));
}

// Collect `ti` and every reflected type reachable through its Struct fields, in a stable discovery
// order, deduped by type_hash. Flattening the type graph like this is what lets one schema blob
// describe a component AND the nested value types it stores (LocalTransform → Transform →
// Vec3/Quat), so the editor can recurse a component's packed bytes into individually editable
// fields.
void collect_types(const core::TypeInfo* ti,
                   std::vector<const core::TypeInfo*>& out,
                   std::unordered_set<std::uint64_t>& seen) {
    if (ti == nullptr || !seen.insert(ti->type_hash).second) {
        return; // null, or a type we already emitted (Vec3 appears in both translation and scale)
    }
    out.push_back(ti);
    for (const core::Field& f : ti->fields) {
        if (f.type == core::FieldType::Struct) {
            collect_types(f.struct_type, out, seen);
        }
    }
}

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
    const ecs::ComponentRegistry& registry = world.components();

    // 1) Gather every reflected type the editor must understand: each registered component, plus
    // the
    //    nested value types those components contain (recursively), deduped by type_hash. We also
    //    remember which hashes are top-level components — the editor's "add component" menu lists
    //    only those, never a bare Vec3.
    std::vector<const core::TypeInfo*> types;
    std::unordered_set<std::uint64_t> seen;
    std::unordered_set<std::uint64_t> component_hashes;
    for (std::size_t i = 0; i < registry.count(); ++i) {
        const ecs::ComponentInfo& info = registry.info(static_cast<ecs::ComponentId>(i));
        if (info.type_info != nullptr) {
            component_hashes.insert(info.type_info->type_hash);
            collect_types(info.type_info, types, seen);
        }
    }

    // 2) Emit the dictionary. Per type: identity, an is-component flag, and its field layout — each
    //    field a name, a kind byte (core::FieldType, wire-stable in declared order), and the nested
    //    type_hash to recurse into for a Struct field (0 for a primitive). The editor pairs this
    //    with a snapshot's opaque blob to decode/edit/re-encode typed values without any per-type
    //    code.
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u32(kSchemaMagic);
    w.u32(static_cast<std::uint32_t>(types.size()));
    for (const core::TypeInfo* ti : types) {
        w.u64(ti->type_hash);
        write_name(w, ti->name);
        w.u8(component_hashes.count(ti->type_hash) != 0 ? 1u : 0u);
        w.u16(static_cast<std::uint16_t>(ti->fields.size()));
        for (const core::Field& f : ti->fields) {
            write_name(w, f.name);
            w.u8(static_cast<std::uint8_t>(f.type));
            w.u64(f.type == core::FieldType::Struct && f.struct_type != nullptr
                      ? f.struct_type->type_hash
                      : 0ull);
        }
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

bool add_default_component(ecs::World& world, ecs::Entity e, std::uint64_t type_hash) {
    if (!world.is_alive(e)) {
        return false;
    }
    const ecs::ComponentId id = id_for_type_hash(world.components(), type_hash);
    if (id == ecs::kInvalidComponentId || world.get_component_raw(e, id) != nullptr) {
        return false; // unknown type, or the entity already has it
    }
    // add_component_raw value-initializes the new slot — the type's real C++ defaults (a zeroed
    // blob would give, e.g., a Transform with scale 0). The editor learns the values on its next
    // snapshot.
    if (world.add_component_raw(e, id) == nullptr) {
        return false;
    }
    world.mark_changed_raw(e, id);
    return true;
}

bool remove_component(ecs::World& world, ecs::Entity e, std::uint64_t type_hash) {
    const ecs::ComponentId id = id_for_type_hash(world.components(), type_hash);
    if (id == ecs::kInvalidComponentId) {
        return false;
    }
    return world.remove_component_raw(e, id);
}

// v1 (m9.5): the browser's asset list. The tag bump is per-message, independent of the schema tag.
constexpr std::uint32_t kAssetListMagic = 0x52414C31u; // 'R''A''L''1' — Rime Asset List v1

std::vector<std::byte> serialize_asset_list(std::span<const AssetListEntry> assets) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    // A string_view is not null-terminated, so write it by length (not via write_name's const
    // char*).
    const auto write_sv = [&w](std::string_view s) {
        w.u16(static_cast<std::uint16_t>(s.size()));
        w.bytes(std::as_bytes(std::span(s.data(), s.size())));
    };
    w.u32(kAssetListMagic);
    w.u32(static_cast<std::uint32_t>(assets.size()));
    for (const AssetListEntry& a : assets) {
        w.u16(a.kind);
        w.u64(a.id);
        write_sv(a.source_path);
        write_sv(a.cooked_file);
    }
    return out;
}

bool spawn_entity_from_payload(ecs::World& world, std::span<const std::byte> payload) {
    core::ByteReader r(payload);
    std::uint16_t comp_count = 0;
    if (!r.u16(comp_count)) {
        return false;
    }
    const ecs::Entity e = world.spawn();
    for (std::uint16_t c = 0; c < comp_count; ++c) {
        std::uint64_t hash = 0;
        std::uint32_t blob_len = 0;
        std::span<const std::byte> blob;
        if (!r.u64(hash) || !r.u32(blob_len) || !r.bytes(blob, blob_len)) {
            break; // truncated — the entity keeps whatever components already applied
        }
        // Reuse the edit path: deserialize each component onto the new entity, adding it. An
        // unknown or malformed component is skipped rather than aborting the whole placement.
        (void)apply_set_component(world, e, hash, blob);
    }
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
        case EditorMessage::AddComponent: {
            core::ByteReader r(payload);
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            std::uint64_t hash = 0;
            if (r.u32(index) && r.u32(generation) && r.u64(hash)) {
                (void)add_default_component(world, ecs::Entity{index, generation}, hash);
            }
            return true;
        }
        case EditorMessage::RemoveComponent: {
            core::ByteReader r(payload);
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            std::uint64_t hash = 0;
            if (r.u32(index) && r.u32(generation) && r.u64(hash)) {
                (void)remove_component(world, ecs::Entity{index, generation}, hash);
            }
            return true;
        }
        case EditorMessage::SpawnEntity:
            (void)spawn_entity_from_payload(world, payload);
            return true;
        case EditorMessage::RequestSnapshot:
            // The editor asks for a fresh view (e.g. after edits, or to refresh); reply with a full
            // snapshot on the same connection. Cheap for editor-sized worlds; a delta channel is a
            // later optimization (nothing else mutates the world while editing — that starts at
            // Play).
            (void)conn_.send_message(static_cast<stream::MessageType>(EditorMessage::Snapshot),
                                     serialize_world(world));
            return true;
        case EditorMessage::PickRequest: {
            // This GPU-free host has no renderer, so there is no ID buffer to hit-test against —
            // but a request left unanswered would strand the client's click. Reply honestly with
            // the "nothing" sentinel (index 0xFFFFFFFF = ecs::kNullEntity's invalid slot index —
            // the same bits an empty-space hit sends). Real picks are the viewport host's job
            // (editor_host_main.cpp, m9.6).
            std::vector<std::byte> out;
            core::ByteWriter w(out);
            w.u32(0xFFFFFFFFu);
            w.u32(0);
            (void)conn_.send_message(static_cast<stream::MessageType>(EditorMessage::PickResult),
                                     out);
            return true;
        }
        default:
            return true; // an engine->editor or unknown type — nothing to apply
    }
}

} // namespace rime::editorhost
