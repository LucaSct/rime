// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/editorhost/editor_host.hpp"

#include <fmt/core.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/core/hash.hpp"
#include "rime/core/reflect/serialize.hpp"
#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/component.hpp"
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

// Despawn every entity currently in `world` — the wipe PlaySession::stop needs before
// deserialize_world, which only ADDS entities (its own doc comment: "Spawns a fresh entity per
// record"); it never clears what is already there. Entities are collected into a vector FIRST: a
// despawn swap-compacts its archetype/chunk (the vacated row is filled by the last one), which
// would skip or re-visit rows if we despawned while walking them directly — the same
// collect-then-mutate discipline editor_host_main.cpp's derive_world_transforms uses for
// add_component.
void despawn_all(ecs::World& world) {
    std::vector<ecs::Entity> alive;
    alive.reserve(world.entity_count());
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t row = 0; row < chunk.size(); ++row) {
                alive.push_back(chunk.entity_at(row));
            }
        }
    }
    for (const ecs::Entity e : alive) {
        world.despawn(e);
    }
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

std::uint64_t world_content_hash(const ecs::World& world) {
    // Same archetype/chunk/row walk as serialize_world, and the same per-component
    // (type_hash, blob) pairs — but folded straight into an FNV-1a running hash rather than a
    // byte buffer, and with NO entity index/generation mixed in (see the header comment: a
    // despawn-everything-then-respawn restore can never reproduce those, by the entity directory's
    // own safety design, so they are not part of "did the data come back exactly").
    const ecs::ComponentRegistry& registry = world.components();
    std::uint64_t h = core::kFnv1a64OffsetBasis;
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        const std::vector<ecs::ComponentId>& ids = arch.signature().ids();
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t row = 0; row < chunk.size(); ++row) {
                for (const ecs::ComponentId id : ids) {
                    const ecs::ComponentInfo& info = registry.info(id);
                    if (info.type_info == nullptr) {
                        continue; // unreflected (e.g. RigidBodyHandle) — not part of the data
                    }
                    const std::uint64_t type_hash = info.type_info->type_hash;
                    h = core::fnv1a_64(std::as_bytes(std::span{&type_hash, 1}), h);
                    const std::vector<std::byte> blob =
                        core::serialize(*info.type_info, chunk.component(id, row));
                    h = core::fnv1a_64(blob, h);
                }
            }
        }
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
    w.u32(msg.index);
    w.u32(msg.generation);
    w.u8(msg.mode);
    w.u8(msg.axis);
    return out;
}

bool parse_gizmo_state(std::span<const std::byte> payload, GizmoStateMsg& out) {
    core::ByteReader r(payload);
    return r.u32(out.index) && r.u32(out.generation) && r.u8(out.mode) && r.u8(out.axis);
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

    const std::string text = scene::save_scene_to_string(world);
    if (!scene::save_scene_file(world, std::filesystem::path(path))) {
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

void PlaySession::play(const ecs::World& world) {
    if (phase_ == PlayPhase::Edit) {
        snapshot_ = serialize_world(world);
        tick_count_ = 0;
    }
    phase_ = PlayPhase::Playing;
}

void PlaySession::pause(const ecs::World& world) {
    if (phase_ == PlayPhase::Edit) {
        snapshot_ = serialize_world(world);
        tick_count_ = 0;
    }
    phase_ = PlayPhase::Paused;
}

bool PlaySession::stop(ecs::World& world) {
    if (phase_ == PlayPhase::Edit) {
        return false; // nothing was playing — no snapshot to restore
    }
    despawn_all(world);
    if (!deserialize_world(world, snapshot_)) {
        // snapshot_ is this session's own serialize_world output, against this same world's
        // registered types, so this should be unreachable; log rather than silently leave `world`
        // however deserialize_world got partway through before the failure.
        RIME_ERROR("editorhost: play-session restore failed to deserialize its own snapshot");
    }
    phase_ = PlayPhase::Edit;
    snapshot_.clear();
    snapshot_.shrink_to_fit();
    tick_count_ = 0;
    return true;
}

bool spawn_entity_from_payload(ecs::World& world, std::span<const std::byte> payload) {
    core::ByteReader r(payload);
    std::uint16_t comp_count = 0;
    if (!r.u16(comp_count)) {
        return false;
    }
    // A PLACEMENT BY DEFAULT, for the same reason bare `Spawn` gets one (m15.3). The asset
    // browser's "place" sends a MeshAsset and nothing else, and an entity with no LocalTransform is
    // one the gizmo will not touch and the renderer will not draw — so "place" put an invisible,
    // unmovable row in the outliner. If the payload carries its own LocalTransform the loop below
    // simply overwrites this one, so an authored placement still wins.
    const ecs::Entity e = world.is_registered<ecs::LocalTransform>()
                              ? world.spawn_with(ecs::LocalTransform{})
                              : world.spawn();
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
            // WITH A TRANSFORM, not empty (m15.3). A bare `world.spawn()` produced an entity the
            // gizmo would not touch (it requires LocalTransform) and the renderer would not draw,
            // so "+ spawn" added an invisible, unmovable row to the outliner and the user's next
            // step was to hunt through "+ add component". A placement is the one thing every
            // spawned entity wants; everything else is genuinely a choice.
            (void)(world.is_registered<ecs::LocalTransform>()
                       ? world.spawn_with(ecs::LocalTransform{})
                       : world.spawn());
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
