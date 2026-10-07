// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/editorhost/editor_host.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/core/hash.hpp"
#include "rime/core/reflect/serialize.hpp"
#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity_refs.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/scene/scene_format.hpp"

// The editor host's implementation. serialize_world / deserialize_world are the reflection walk:
// for every entity, for every *reflected* component, serialize its bytes through its TypeInfo.
// Nothing here names a concrete component type — a registered component is snapshot and edited for
// free (ADR-0018 §4 / ADR-0031). The EditorHost wraps that over a ProtocolConnection.
namespace rime::editorhost {
namespace {

// A snapshot/schema blob starts with this tag so a truncated or wrong-shaped buffer is rejected at
// the first read, not mis-parsed. (Bumped only if the editor blob layout changes.)
// v2 (ADR-0075): each entity record carries its EditorId after the handle, and the editor's copy
// writes Entity fields as EditorIds. The tag bump makes a v1 editor reject the new shape.
constexpr std::uint32_t kSnapshotMagic = 0x52534E32u; // 'R''S''N''2' — Rime SNapshot v2
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

// Every live entity, in archetype/chunk/row order. Collected into a vector so a caller can mutate
// the world while going through them: a structural change swap-compacts a chunk (the vacated row
// is filled by the last one), which would skip or revisit rows if we mutated while walking.
std::vector<ecs::Entity> live_entities(const ecs::World& world) {
    std::vector<ecs::Entity> out;
    out.reserve(world.entity_count());
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t row = 0; row < chunk.size(); ++row) {
                out.push_back(chunk.entity_at(row));
            }
        }
    }
    return out;
}

// On the editor wire an Entity field holds an EditorId (ADR-0075). `Entity` is reflected as
// {index:u32, generation:u32}, which reflection serializes as two little-endian u32s — the same 8
// bytes as one little-endian u64 whose low half is `index`. So an id is carried by storing its low
// and high halves in those two fields, and the editor reads the field as a single u64.
ecs::Entity pack_editor_id(std::uint64_t id) {
    return ecs::Entity{static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(id >> 32)};
}

std::uint64_t unpack_editor_id(ecs::Entity e) {
    return static_cast<std::uint64_t>(e.index) | (static_cast<std::uint64_t>(e.generation) << 32);
}

// Rewrite one handle into the id the editor knows it by: null → 0, a live entity → its EditorId, a
// dead handle → kDanglingEditorId (counted — it is a fact about the world worth seeing, and the
// editor sending it back gets refused rather than resolved to something else).
ecs::Entity handle_to_wire(const ecs::World& world, EditorIds* ids, ecs::Entity e) {
    if (e == ecs::kNullEntity) {
        return pack_editor_id(kNoEditorId);
    }
    const std::uint64_t id = editor_id_of(world, e);
    if (id == kNoEditorId) {
        if (ids != nullptr) {
            ++ids->counters().unresolved_refs;
        }
        RIME_ERROR("editorhost: a component references {}:{}, which is not a live entity with an "
                   "EditorId — sent as dangling",
                   e.index,
                   e.generation);
        return pack_editor_id(kDanglingEditorId);
    }
    return pack_editor_id(id);
}

// One entity record of the RSN2 layout (see serialize_world). `ids` non-null = the editor's copy:
// Entity fields become EditorIds. EditorId itself is in the header, so it is not listed again.
void write_entity_record(core::ByteWriter& w,
                         const ecs::World& world,
                         ecs::Entity e,
                         EditorIds* ids) {
    const ecs::ComponentRegistry& registry = world.components();
    w.u32(e.index);
    w.u32(e.generation);
    w.u64(editor_id_of(world, e));
    const std::vector<ecs::ComponentId>& sig = world.signature_of(e).ids();
    std::uint16_t comp_count = 0;
    for (const ecs::ComponentId id : sig) {
        const ecs::ComponentInfo& info = registry.info(id);
        if (info.type_info != nullptr && !info.session) {
            ++comp_count;
        }
    }
    w.u16(comp_count);
    for (const ecs::ComponentId id : sig) {
        const ecs::ComponentInfo& info = registry.info(id);
        if (info.type_info == nullptr || info.session) {
            continue;
        }
        std::vector<std::byte> blob =
            core::serialize(*info.type_info, world.get_component_raw(e, id));
        if (ids != nullptr) {
            (void)rewrite_blob_entity_refs(info, blob, [&](ecs::Entity& ref) {
                ref = handle_to_wire(world, ids, ref);
                return true;
            });
        }
        w.u64(info.type_info->type_hash);
        w.u32(static_cast<std::uint32_t>(blob.size()));
        w.bytes(blob);
    }
}

// Translate an incoming blob's Entity fields (EditorIds) to live handles. A null stays null; an id
// nothing holds is REFUSED (false, counted, logged) — never quietly nulled.
bool blob_from_wire(ecs::World& world,
                    EditorIds& ids,
                    const ecs::ComponentInfo& info,
                    std::vector<std::byte>& blob) {
    return rewrite_blob_entity_refs(info, blob, [&](ecs::Entity& ref) {
        const std::uint64_t id = unpack_editor_id(ref);
        if (id == kNoEditorId) {
            ref = ecs::kNullEntity;
            return true;
        }
        const ecs::Entity e = ids.resolve(world, id);
        if (e == ecs::kNullEntity) {
            ++ids.counters().unresolved_refs;
            RIME_ERROR(
                "editorhost: refusing a {} that references EditorId {}, which no live entity "
                "holds",
                info.name,
                id);
            return false;
        }
        ref = e;
        return true;
    });
}

} // namespace

bool is_editor_message(stream::MessageType type) noexcept {
    const auto v = static_cast<std::uint16_t>(type);
    return v >= static_cast<std::uint16_t>(stream::MessageType::EditorReservedBegin) &&
           v <= static_cast<std::uint16_t>(stream::MessageType::EditorReservedEnd);
}

bool message_affects_frame(EditorMessage msg) noexcept {
    switch (msg) {
        // World edits (the camera is a world entity, so a camera move is a SetComponent), the gizmo
        // overlay (composited into the frame), and the play-state transitions all change the next
        // streamed frame — so the render/capture/encode/send pipeline must run this iteration.
        case EditorMessage::SetComponent:
        case EditorMessage::Spawn:
        case EditorMessage::Despawn:
        case EditorMessage::AddComponent:
        case EditorMessage::RemoveComponent:
        case EditorMessage::SpawnEntity:
        case EditorMessage::GizmoState:
        case EditorMessage::Play:
        case EditorMessage::Pause:
        case EditorMessage::Step:
        case EditorMessage::Stop:
            return true;
        // Not frame-affecting. RequestSnapshot is answered with a Snapshot (world bytes, not a
        // frame); PickRequest is served by the independent 1×1 pick pass. The engine->editor kinds
        // are never received here, but a total switch classifies them for completeness.
        case EditorMessage::RequestSnapshot:
        case EditorMessage::PickRequest:
        // SaveScene writes a file and answers with a SaveResult. It changes nothing in the world,
        // so it must not wake the render loop — an editor that saves every few seconds would
        // otherwise defeat the whole idle-skip (m10.0-perf: "idle work is a bug").
        case EditorMessage::SaveScene:
        case EditorMessage::SaveResult:
        case EditorMessage::EditResult:
        case EditorMessage::SceneLoadReport:
        case EditorMessage::Schema:
        case EditorMessage::Snapshot:
        case EditorMessage::AssetList:
        case EditorMessage::PickResult:
        case EditorMessage::ViewportCamera:
        case EditorMessage::PlayState:
            return false;
    }
    // A value outside the enumerator set (an out-of-band cast) — the receiver already drops unknown
    // types, so treat it as no-op rather than waking the pipeline.
    return false;
}

std::vector<std::byte> serialize_world(const ecs::World& world) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u32(kSnapshotMagic);
    w.u32(static_cast<std::uint32_t>(world.entity_count()));
    for (const ecs::Entity e : live_entities(world)) {
        write_entity_record(w, world, e, nullptr);
    }
    return out;
}

std::vector<std::byte> serialize_editor_snapshot(ecs::World& world, EditorIds& ids) {
    // An entity the editor cannot name is an entity it cannot select, edit or undo — so anything
    // that appeared without an id (game code spawning during Play) gets one before it is shown.
    (void)ids.assign_missing(world);
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u32(kSnapshotMagic);
    w.u32(static_cast<std::uint32_t>(world.entity_count()));
    for (const ecs::Entity e : entities_in_editor_order(world)) {
        write_entity_record(w, world, e, &ids);
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

    struct Record {
        ecs::Entity old_handle;
        std::uint64_t editor_id;
        std::vector<std::pair<std::uint64_t, std::span<const std::byte>>> comps;
    };

    // Phase 1: parse every record, so phase 2 can spawn them all before any component is filled —
    // the scene loader's discipline, and what makes a forward reference resolvable.
    std::vector<Record> records;
    records.reserve(entity_count);
    for (std::uint32_t i = 0; i < entity_count; ++i) {
        Record rec{};
        std::uint16_t comp_count = 0;
        if (!r.u32(rec.old_handle.index) || !r.u32(rec.old_handle.generation) ||
            !r.u64(rec.editor_id) || !r.u16(comp_count)) {
            RIME_ERROR("editorhost: truncated snapshot entity {}", i);
            return false;
        }
        for (std::uint16_t c = 0; c < comp_count; ++c) {
            std::uint64_t hash = 0;
            std::uint32_t blob_len = 0;
            std::span<const std::byte> blob;
            if (!r.u64(hash) || !r.u32(blob_len) || !r.bytes(blob, blob_len)) {
                RIME_ERROR("editorhost: truncated snapshot component");
                return false;
            }
            rec.comps.emplace_back(hash, blob);
        }
        records.push_back(std::move(rec));
    }
    // Phase 2: a fresh entity per record, and the old-handle → new-handle map references go
    // through.
    std::unordered_map<std::uint64_t, ecs::Entity> remap;
    std::vector<ecs::Entity> spawned;
    spawned.reserve(records.size());
    const auto key = [](ecs::Entity e) { return unpack_editor_id(e); }; // the handle's 64 bits
    for (const Record& rec : records) {
        spawned.push_back(dst.spawn());
        remap.emplace(key(rec.old_handle), spawned.back());
    }
    // Phase 3: fill, rewriting every Entity field through the map.
    const ecs::ComponentRegistry& registry = dst.components();
    bool ok = true;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const ecs::Entity e = spawned[i];
        if (records[i].editor_id != kNoEditorId && dst.is_registered<EditorId>()) {
            (void)dst.add_component(e, EditorId{records[i].editor_id});
        }
        for (const auto& [hash, blob] : records[i].comps) {
            const ecs::ComponentId id = id_for_type_hash(registry, hash);
            if (id == ecs::kInvalidComponentId) {
                RIME_ERROR("editorhost: snapshot has unknown component type_hash {:#x}", hash);
                return false;
            }
            const ecs::ComponentInfo& info = registry.info(id);
            void* slot = dst.add_component_raw(e, id);
            if (slot == nullptr || !core::deserialize(*info.type_info, slot, blob)) {
                RIME_ERROR("editorhost: failed to apply snapshot component");
                return false;
            }
            (void)ecs::for_each_entity_ref(
                *info.type_info, static_cast<std::byte*>(slot), [&](ecs::Entity& ref) {
                    if (ref == ecs::kNullEntity) {
                        return true;
                    }
                    if (const auto it = remap.find(key(ref)); it != remap.end()) {
                        ref = it->second;
                    } else {
                        RIME_ERROR("editorhost: snapshot {} references {}:{}, which the snapshot "
                                   "does not contain",
                                   info.name,
                                   ref.index,
                                   ref.generation);
                        ok = false;
                    }
                    return true; // keep walking: report every unresolved field, not just the first
                });
        }
    }
    return ok;
}

std::uint64_t world_content_hash(const ecs::World& world) {
    // Per entity: FNV-1a over its (type_hash, blob) pairs in signature order — with Entity fields
    // replaced by the referent's EditorId where it has one, so identity rather than storage is
    // hashed. Then the digests are sorted and folded: a multiset hash, blind to which row or
    // archetype an entity sits in (see the header for why that is the right blindness).
    const ecs::ComponentRegistry& registry = world.components();
    std::vector<std::uint64_t> digests;
    digests.reserve(world.entity_count());
    for (const ecs::Entity e : live_entities(world)) {
        std::uint64_t h = core::kFnv1a64OffsetBasis;
        bool any = false;
        for (const ecs::ComponentId id : world.signature_of(e).ids()) {
            const ecs::ComponentInfo& info = registry.info(id);
            if (info.type_info == nullptr) {
                continue; // unreflected (e.g. RigidBodyHandle) — not part of the data
            }
            any = true;
            const std::uint64_t type_hash = info.type_info->type_hash;
            h = core::fnv1a_64(std::as_bytes(std::span{&type_hash, 1}), h);
            std::vector<std::byte> blob =
                core::serialize(*info.type_info, world.get_component_raw(e, id));
            (void)rewrite_blob_entity_refs(info, blob, [&](ecs::Entity& ref) {
                const std::uint64_t ref_id = editor_id_of(world, ref);
                if (ref_id != kNoEditorId) {
                    ref = pack_editor_id(ref_id);
                }
                return true;
            });
            h = core::fnv1a_64(blob, h);
        }
        if (any) { // an entity with no reflected component carries no data to fingerprint
            digests.push_back(h);
        }
    }
    std::sort(digests.begin(), digests.end());
    std::uint64_t h = core::kFnv1a64OffsetBasis;
    for (const std::uint64_t d : digests) {
        h = core::fnv1a_64(std::as_bytes(std::span{&d, 1}), h);
    }
    return h;
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
        // Session components (EditorId) are the host's bookkeeping, not something to inspect or
        // add — the editor learns an entity's id from the snapshot's entity header instead.
        if (info.type_info != nullptr && !info.session) {
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
        if (ti == &core::reflect<ecs::Entity>()) {
            // On the editor wire an entity reference IS its EditorId (ADR-0075): the same 8 bytes
            // as {index, generation}, read as one u64. Describing it that way is what makes the
            // inspector show, and edit, the name the editor actually uses.
            w.u16(1);
            write_name(w, "editor_id");
            w.u8(static_cast<std::uint8_t>(core::FieldType::UInt64));
            w.u64(0);
            continue;
        }
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

std::vector<std::byte> serialize_viewport_camera(const ViewportCameraMsg& msg) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    // Element by element through the f32 helper (not a memcpy of the structs): the wire is defined
    // as a sequence of little-endian IEEE floats, and the helper is what guarantees that on every
    // host — the same discipline every other payload here follows.
    for (const float e : msg.view_proj) {
        w.f32(e);
    }
    for (const float e : msg.inv_view_proj) {
        w.f32(e);
    }
    for (const float e : msg.eye) {
        w.f32(e);
    }
    w.u32(msg.width);
    w.u32(msg.height);
    return out;
}

bool parse_viewport_camera(std::span<const std::byte> payload, ViewportCameraMsg& out) {
    core::ByteReader r(payload);
    for (float& e : out.view_proj) {
        if (!r.f32(e)) {
            return false;
        }
    }
    for (float& e : out.inv_view_proj) {
        if (!r.f32(e)) {
            return false;
        }
    }
    for (float& e : out.eye) {
        if (!r.f32(e)) {
            return false;
        }
    }
    return r.u32(out.width) && r.u32(out.height);
}

std::vector<std::byte> serialize_gizmo_state(const GizmoStateMsg& msg) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u64(msg.editor_id);
    w.u8(msg.mode);
    w.u8(msg.axis);
    return out;
}

bool parse_gizmo_state(std::span<const std::byte> payload, GizmoStateMsg& out) {
    core::ByteReader r(payload);
    return r.u64(out.editor_id) && r.u8(out.mode) && r.u8(out.axis);
}

std::vector<std::byte> serialize_play_state(const PlayStateMsg& msg) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u8(static_cast<std::uint8_t>(msg.phase));
    w.u64(msg.tick_count);
    return out;
}

bool parse_play_state(std::span<const std::byte> payload, PlayStateMsg& out) {
    core::ByteReader r(payload);
    std::uint8_t phase = 0;
    if (!r.u8(phase) || !r.u64(out.tick_count)) {
        return false;
    }
    out.phase = static_cast<PlayPhase>(phase);
    return true;
}

// ── Saving the world back to a `.rscene` (m14.3) ─────────────────────────────────────────

namespace {

void write_string(core::ByteWriter& w, std::string_view s) {
    w.u32(static_cast<std::uint32_t>(s.size()));
    for (const char c : s) {
        w.u8(static_cast<std::uint8_t>(c));
    }
}

bool read_string(core::ByteReader& r, std::string& out) {
    std::uint32_t len = 0;
    if (!r.u32(len)) {
        return false;
    }
    std::span<const std::byte> bytes;
    if (!r.bytes(bytes, len)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

} // namespace

SceneSaveOutcome save_hosted_scene(const ecs::World& world,
                                   const HostedScene& hosted,
                                   std::string_view requested_path) {
    SceneSaveOutcome out;

    // THE REFUSAL, and it is the whole reason this is a function rather than three inline lines.
    // The editor loads leniently so it can open a scene authored by a build it does not match
    // (scene::LoadOptions, m14.1). What is in memory is therefore missing whatever it could not
    // read, and writing that back deletes those records from the file — silently, permanently, with
    // every counter green. Tolerance at load is only safe while this veto exists at save.
    if (hosted.skipped_components != 0) {
        out.error = fmt::format(
            "refusing to save: this build did not understand {} component(s) in the scene it "
            "loaded, and saving would delete them. Open it in a build that registers them.",
            hosted.skipped_components);
        return out;
    }

    const std::string path(requested_path.empty() ? hosted.path : std::string(requested_path));
    if (path.empty()) {
        // A built-in world has no file to go back to, and inventing one would put a scene somewhere
        // nobody asked for.
        out.error = "refusing to save: no path given and this world was not loaded from a file";
        return out;
    }

    // Editor order, not storage order: see the header. The one place the host's identity reaches a
    // file, and it reaches it only as an ORDER — the ids themselves are never written.
    const std::vector<ecs::Entity> order = entities_in_editor_order(world);
    const std::string text = scene::save_scene_to_string(world, order);
    if (!scene::save_scene_file(world, std::filesystem::path(path), order)) {
        out.error = "could not write scene file: " + path;
        return out;
    }

    out.ok = true;
    out.path = path;
    out.entities = world.entity_count();
    out.bytes = text.size();
    return out;
}

std::vector<std::byte> serialize_save_scene(std::string_view path) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    write_string(w, path);
    return out;
}

bool parse_save_scene(std::span<const std::byte> payload, std::string& out_path) {
    core::ByteReader r(payload);
    return read_string(r, out_path);
}

std::vector<std::byte> serialize_save_result(const SceneSaveOutcome& outcome) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u8(outcome.ok ? 1u : 0u);
    w.u64(static_cast<std::uint64_t>(outcome.entities));
    w.u64(static_cast<std::uint64_t>(outcome.bytes));
    write_string(w, outcome.path);
    write_string(w, outcome.error);
    return out;
}

bool parse_save_result(std::span<const std::byte> payload, SceneSaveOutcome& out) {
    core::ByteReader r(payload);
    std::uint8_t ok = 0;
    std::uint64_t entities = 0;
    std::uint64_t bytes = 0;
    if (!r.u8(ok) || !r.u64(entities) || !r.u64(bytes) || !read_string(r, out.path) ||
        !read_string(r, out.error)) {
        return false;
    }
    out.ok = ok != 0;
    out.entities = static_cast<std::size_t>(entities);
    out.bytes = static_cast<std::size_t>(bytes);
    return true;
}

std::vector<std::byte> serialize_scene_load_report(const HostedScene& hosted) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u8(hosted.load_ok ? 1u : 0u);
    w.u32(static_cast<std::uint32_t>(hosted.skipped_components));
    write_string(w, hosted.requested_path);
    write_string(w, hosted.load_error);
    return out;
}

bool parse_scene_load_report(std::span<const std::byte> payload, SceneLoadReport& out) {
    core::ByteReader r(payload);
    std::uint8_t ok = 0;
    std::uint32_t skipped = 0;
    if (!r.u8(ok) || !r.u32(skipped) || !read_string(r, out.path) || !read_string(r, out.error)) {
        return false;
    }
    out.ok = ok != 0;
    out.skipped_components = skipped;
    return true;
}

void PlaySession::take_baseline(const ecs::World& world) {
    const ecs::ComponentRegistry& registry = world.components();
    baseline_.clear();
    baseline_.reserve(world.entity_count());
    for (const ecs::Entity e : live_entities(world)) {
        BaselineEntity rec;
        rec.handle = e;
        rec.editor_id = editor_id_of(world, e);
        rec.signature = world.signature_of(e).ids();
        for (const ecs::ComponentId id : rec.signature) {
            const ecs::ComponentInfo& info = registry.info(id);
            if (info.type_info != nullptr) {
                rec.values.emplace_back(
                    id, core::serialize(*info.type_info, world.get_component_raw(e, id)));
            }
        }
        baseline_.push_back(std::move(rec));
    }
}

void PlaySession::play(const ecs::World& world) {
    if (phase_ == PlayPhase::Edit) {
        take_baseline(world);
        tick_count_ = 0;
    }
    phase_ = PlayPhase::Playing;
}

void PlaySession::pause(const ecs::World& world) {
    if (phase_ == PlayPhase::Edit) {
        take_baseline(world);
        tick_count_ = 0;
    }
    phase_ = PlayPhase::Paused;
}

bool PlaySession::stop(ecs::World& world) {
    if (phase_ == PlayPhase::Edit) {
        return false; // nothing was playing — no baseline to restore
    }
    StopReport report;
    const ecs::ComponentRegistry& registry = world.components();
    const auto handle_key = [](ecs::Entity e) { return unpack_editor_id(e); }; // its 64 bits

    // 1. Newcomers out. Anything alive that the baseline does not name was created during play.
    //    (A newcomer can sit in a slot a baseline entity vacated, but never under that entity's
    //    handle: the despawn bumped the generation. So "same handle" is an exact survivor test.)
    std::unordered_set<std::uint64_t> baseline_handles;
    baseline_handles.reserve(baseline_.size());
    for (const BaselineEntity& rec : baseline_) {
        baseline_handles.insert(handle_key(rec.handle));
    }
    for (const ecs::Entity e : live_entities(world)) {
        if (baseline_handles.count(handle_key(e)) == 0) {
            (void)world.despawn(e);
            ++report.removed_newcomers;
        }
    }

    // 2. Allocate before filling. A survivor keeps its handle; a casualty gets a FRESH one — its
    // old
    //    handle stays dead, because rewinding a generation would revive every stale copy of it.
    //    References then go old handle → identity → new handle, where identity is the EditorId
    //    (the editor's name for the entity). An entity no editor host stamped has no EditorId; its
    //    old handle stands in as its identity, kept in a separate map so the two key spaces can
    //    never collide.
    std::unordered_map<std::uint64_t, std::uint64_t> editor_id_of_old; // old handle → EditorId
    std::unordered_map<std::uint64_t, ecs::Entity> new_of_editor_id;   // EditorId → new handle
    std::unordered_map<std::uint64_t, ecs::Entity> new_of_unnamed;     // old handle → new handle
    std::vector<ecs::Entity> target(baseline_.size());
    for (std::size_t i = 0; i < baseline_.size(); ++i) {
        const BaselineEntity& rec = baseline_[i];
        if (world.is_alive(rec.handle)) {
            target[i] = rec.handle;
            ++report.survivors;
        } else {
            target[i] = world.spawn();
            ++report.respawned;
        }
        if (rec.editor_id != kNoEditorId) {
            editor_id_of_old.emplace(handle_key(rec.handle), rec.editor_id);
            new_of_editor_id.emplace(rec.editor_id, target[i]);
        } else {
            new_of_unnamed.emplace(handle_key(rec.handle), target[i]);
        }
    }
    const auto new_handle_for = [&](ecs::Entity old) -> ecs::Entity {
        if (const auto it = editor_id_of_old.find(handle_key(old)); it != editor_id_of_old.end()) {
            return new_of_editor_id.at(it->second);
        }
        if (const auto it = new_of_unnamed.find(handle_key(old)); it != new_of_unnamed.end()) {
            return it->second;
        }
        return ecs::kNullEntity;
    };

    // 3. Fill. Exactly the baseline component SET (remove what play added; re-add what it removed),
    //    then exactly the baseline VALUES, with every Entity field rewritten through the map.
    for (std::size_t i = 0; i < baseline_.size(); ++i) {
        const BaselineEntity& rec = baseline_[i];
        const ecs::Entity e = target[i];
        std::vector<ecs::ComponentId> extra;
        for (const ecs::ComponentId id : world.signature_of(e).ids()) {
            if (std::find(rec.signature.begin(), rec.signature.end(), id) == rec.signature.end()) {
                extra.push_back(id);
            }
        }
        for (const ecs::ComponentId id : extra) {
            (void)world.remove_component_raw(e, id); // e.g. the RigidBodyHandle play bound
        }
        for (const ecs::ComponentId id : rec.signature) {
            if (world.get_component_raw(e, id) == nullptr) {
                (void)world.add_component_raw(e, id); // value-initialized; derived state re-derives
            }
        }
        for (const auto& [id, blob] : rec.values) {
            const ecs::ComponentInfo& info = registry.info(id);
            void* slot = world.get_component_raw(e, id);
            if (slot == nullptr || !core::deserialize(*info.type_info, slot, blob)) {
                // The baseline is this session's own serialization of this world's own types, so
                // this should be unreachable; say so loudly rather than leave a half-restored row.
                RIME_ERROR("editorhost: stop could not restore a {} from its own baseline",
                           info.name);
                continue;
            }
            (void)ecs::for_each_entity_ref(
                *info.type_info, static_cast<std::byte*>(slot), [&](ecs::Entity& ref) {
                    if (ref == ecs::kNullEntity) {
                        return true;
                    }
                    const ecs::Entity now = new_handle_for(ref);
                    if (now == ecs::kNullEntity) {
                        // Dangling BEFORE play (it named nothing in the baseline). Restored as it
                        // was — still dangling, never silently nulled — and counted.
                        ++report.refs_unresolved;
                        RIME_ERROR("editorhost: stop restored a {} whose reference {}:{} named no "
                                   "entity before play either",
                                   info.name,
                                   ref.index,
                                   ref.generation);
                        return true;
                    }
                    if (now != ref) {
                        ref = now;
                        ++report.refs_remapped;
                    }
                    return true;
                });
            world.mark_changed_raw(e, id);
        }
    }

    last_stop_ = report;
    phase_ = PlayPhase::Edit;
    baseline_.clear();
    baseline_.shrink_to_fit();
    tick_count_ = 0;
    return true;
}

// ── Identity-resolving edits (ADR-0075) ──────────────────────────────────────────────────────

std::size_t despawn_nulling_references(ecs::World& world, ecs::Entity target) {
    if (!world.is_alive(target)) {
        return 0;
    }
    const ecs::ComponentRegistry& registry = world.components();
    std::size_t nulled = 0;
    for (const ecs::Entity e : live_entities(world)) {
        if (e == target) {
            continue; // its own self-reference goes with it
        }
        for (const ecs::ComponentId id : world.signature_of(e).ids()) {
            const ecs::ComponentInfo& info = registry.info(id);
            if (info.type_info == nullptr || !ecs::has_entity_refs(*info.type_info)) {
                continue;
            }
            std::size_t here = 0;
            auto* base = static_cast<std::byte*>(world.get_component_raw(e, id));
            (void)ecs::for_each_entity_ref(*info.type_info, base, [&](ecs::Entity& ref) {
                if (ref == target) {
                    ref = ecs::kNullEntity;
                    ++here;
                }
                return true;
            });
            if (here != 0) {
                world.mark_changed_raw(e, id);
                nulled += here;
            }
        }
    }
    (void)world.despawn(target);
    return nulled;
}

namespace {

// Resolve the entity a command names, counting and logging a miss — the one refusal path every
// targeted command shares.
ecs::Entity
resolve_target(ecs::World& world, EditorIds& ids, std::uint64_t editor_id, const char* what) {
    const ecs::Entity e = ids.resolve(world, editor_id);
    if (e == ecs::kNullEntity) {
        ++ids.counters().unresolved_ids;
        RIME_ERROR("editorhost: refusing {} — no live entity has EditorId {}", what, editor_id);
    }
    return e;
}

// Spawn (and SpawnEntity) — fresh, or under a previously issued id (a redo, or an undone
// despawn). The id is checked BEFORE anything is created, and stamped BEFORE any component is
// filled, so a component that references its own entity resolves.
EditOutcome spawn_with_identity(ecs::World& world, EditorIds& ids, const SpawnEntityMsg& msg) {
    EditOutcome out{.reply = true, .ok = false, .editor_id = msg.editor_id};
    if (msg.editor_id != kNoEditorId && !ids.can_adopt(world, msg.editor_id)) {
        return out;
    }
    // A PLACEMENT BY DEFAULT (m15.3) unless this is an exact restore. "+ spawn" and the asset
    // browser's "place" both used to make an entity with no LocalTransform — one the gizmo will not
    // touch and the renderer will not draw. If the payload carries its own LocalTransform the loop
    // below simply overwrites this one, so an authored placement still wins.
    const ecs::Entity e = !msg.exact && world.is_registered<ecs::LocalTransform>()
                              ? world.spawn_with(ecs::LocalTransform{})
                              : world.spawn();
    if (msg.editor_id == kNoEditorId) {
        out.editor_id = ids.assign(world, e);
    } else {
        ids.adopt(world, e, msg.editor_id);
    }
    const ecs::ComponentRegistry& registry = world.components();
    for (const auto& [hash, wire_blob] : msg.components) {
        const ecs::ComponentId id = id_for_type_hash(registry, hash);
        if (id == ecs::kInvalidComponentId || registry.info(id).session) {
            // Unknown or not editor-settable: skipped, the entity still spawns (m9.5's rule).
            RIME_WARN("editorhost: spawn skipped an unknown component type_hash {:#x}", hash);
            continue;
        }
        std::vector<std::byte> blob = wire_blob;
        if (!blob_from_wire(world, ids, registry.info(id), blob)) {
            // A reference that resolves to nothing: refuse the WHOLE spawn rather than create an
            // entity missing part of what the editor will believe it has.
            (void)world.despawn(e);
            return out;
        }
        (void)apply_set_component(world, e, hash, blob);
    }
    out.ok = true;
    return out;
}

} // namespace

EditOutcome apply_editor_edit(ecs::World& world,
                              EditorIds& ids,
                              EditorMessage type,
                              std::span<const std::byte> payload) {
    const ecs::ComponentRegistry& registry = world.components();
    switch (type) {
        case EditorMessage::SetComponent: {
            // Not acknowledged: it is the high-rate message (every frame of a drag), and its
            // history step is the gesture, recorded by the editor at gesture end.
            SetComponentMsg msg;
            if (!parse_set_component(payload, msg)) {
                RIME_ERROR("editorhost: malformed SetComponent");
                return {};
            }
            const ecs::Entity e = resolve_target(world, ids, msg.editor_id, "SetComponent");
            const ecs::ComponentId id = id_for_type_hash(registry, msg.type_hash);
            if (e == ecs::kNullEntity || id == ecs::kInvalidComponentId ||
                registry.info(id).session) {
                return {};
            }
            if (blob_from_wire(world, ids, registry.info(id), msg.blob)) {
                (void)apply_set_component(world, e, msg.type_hash, msg.blob);
            }
            return {};
        }
        case EditorMessage::Spawn: {
            SpawnEntityMsg msg;
            if (!payload.empty() && !parse_entity_ref(payload, msg.editor_id)) {
                RIME_ERROR("editorhost: malformed Spawn");
                return {.reply = true, .ok = false, .editor_id = kNoEditorId};
            }
            return spawn_with_identity(world, ids, msg);
        }
        case EditorMessage::SpawnEntity: {
            SpawnEntityMsg msg;
            if (!parse_spawn_entity(payload, msg)) {
                RIME_ERROR("editorhost: malformed SpawnEntity");
                return {.reply = true, .ok = false, .editor_id = kNoEditorId};
            }
            return spawn_with_identity(world, ids, msg);
        }
        case EditorMessage::Despawn: {
            EditOutcome out{.reply = true, .ok = false, .editor_id = kNoEditorId};
            if (!parse_entity_ref(payload, out.editor_id)) {
                RIME_ERROR("editorhost: malformed Despawn");
                return out;
            }
            const ecs::Entity e = resolve_target(world, ids, out.editor_id, "Despawn");
            if (e != ecs::kNullEntity) {
                ids.counters().nulled_refs += despawn_nulling_references(world, e);
                out.ok = true;
            }
            return out;
        }
        case EditorMessage::AddComponent:
        case EditorMessage::RemoveComponent: {
            const bool add = type == EditorMessage::AddComponent;
            const char* what = add ? "AddComponent" : "RemoveComponent";
            ComponentRefMsg msg;
            EditOutcome out{.reply = true, .ok = false, .editor_id = kNoEditorId};
            if (!parse_component_ref(payload, msg)) {
                RIME_ERROR("editorhost: malformed {}", what);
                return out;
            }
            out.editor_id = msg.editor_id;
            const ecs::Entity e = resolve_target(world, ids, msg.editor_id, what);
            const ecs::ComponentId id = id_for_type_hash(registry, msg.type_hash);
            if (e == ecs::kNullEntity || id == ecs::kInvalidComponentId ||
                registry.info(id).session) {
                return out;
            }
            out.ok = add ? add_default_component(world, e, msg.type_hash)
                         : remove_component(world, e, msg.type_hash);
            return out;
        }
        default:
            return {}; // not a world edit
    }
}

// ── EditorHost ──────────────────────────────────────────────────────────────────────────

EditorHost::EditorHost(stream::ProtocolConnection conn) noexcept : conn_(std::move(conn)) {}

bool EditorHost::send_hello(ecs::World& world) {
    (void)ids_.assign_missing(world); // before the schema, so EditorId is registered (and hidden)
    const std::vector<std::byte> schema = serialize_schema(world);
    const std::vector<std::byte> report = serialize_scene_load_report(hosted_);
    const std::vector<std::byte> snapshot = serialize_editor_snapshot(world, ids_);
    return conn_.send_message(static_cast<stream::MessageType>(EditorMessage::Schema), schema) &&
           conn_.send_message(static_cast<stream::MessageType>(EditorMessage::SceneLoadReport),
                              report) &&
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
    const auto msg = static_cast<EditorMessage>(type);
    switch (msg) {
        case EditorMessage::SetComponent:
        case EditorMessage::Spawn:
        case EditorMessage::SpawnEntity:
        case EditorMessage::Despawn:
        case EditorMessage::AddComponent:
        case EditorMessage::RemoveComponent: {
            // One dispatcher for both hosts (apply_editor_edit), and the structural edits answered
            // in arrival order — the editor commits an undo step only on that answer.
            const EditOutcome outcome = apply_editor_edit(world, ids_, msg, payload);
            if (outcome.reply) {
                (void)conn_.send_message(
                    static_cast<stream::MessageType>(EditorMessage::EditResult),
                    serialize_edit_result({.ok = outcome.ok, .editor_id = outcome.editor_id}));
            }
            return true;
        }
        case EditorMessage::RequestSnapshot:
            // The editor asks for a fresh view (e.g. after edits, or to refresh); reply with a full
            // snapshot on the same connection. Cheap for editor-sized worlds; a delta channel is a
            // later optimization (nothing else mutates the world while editing — that starts at
            // Play).
            (void)conn_.send_message(static_cast<stream::MessageType>(EditorMessage::Snapshot),
                                     serialize_editor_snapshot(world, ids_));
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
        case EditorMessage::SaveScene: {
            // The engine writes the file (scene_format.hpp: the C++ writer is the reference
            // implementation and the Rust editor reuses it through files). The editor is told the
            // outcome either way — including a refusal, which must carry its reason or it is
            // indistinguishable from a bug.
            std::string path;
            if (!parse_save_scene(payload, path)) {
                RIME_ERROR("editorhost: malformed SaveScene");
                return true;
            }
            const SceneSaveOutcome outcome = save_hosted_scene(world, hosted_, path);
            if (outcome.ok) {
                RIME_INFO("editorhost: saved {} entities to {} ({} bytes)",
                          outcome.entities,
                          outcome.path,
                          outcome.bytes);
            } else {
                RIME_WARN("editorhost: save refused — {}", outcome.error);
            }
            (void)conn_.send_message(static_cast<stream::MessageType>(EditorMessage::SaveResult),
                                     serialize_save_result(outcome));
            return true;
        }
        default:
            return true; // an engine->editor or unknown type — nothing to apply
    }
}

} // namespace rime::editorhost
