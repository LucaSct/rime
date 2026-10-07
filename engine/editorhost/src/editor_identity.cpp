// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/editorhost/editor_identity.hpp"

#include <algorithm>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/world.hpp"

// The editor host's identity registry and the id-carrying wire payloads (ADR-0075). See the header
// for the idea; the notes here are about the mechanics.
namespace rime::editorhost {

void register_editor_components(ecs::World& world) {
    (void)world.register_component<EditorId>();
}

std::uint64_t editor_id_of(const ecs::World& world, ecs::Entity e) noexcept {
    if (!world.is_registered<EditorId>() || !world.is_alive(e)) {
        return kNoEditorId;
    }
    const EditorId* id = world.get<EditorId>(e);
    return id != nullptr ? id->value : kNoEditorId;
}

std::vector<ecs::Entity> entities_in_editor_order(const ecs::World& world) {
    // (id, iteration position, entity). The position is the tiebreak that keeps id-less entities
    // (id 0, sorted after every real id by the key below) in their stable iteration order.
    struct Row {
        std::uint64_t key;
        std::size_t position;
        ecs::Entity e;
    };

    std::vector<Row> rows;
    rows.reserve(world.entity_count());
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t r = 0; r < chunk.size(); ++r) {
                const ecs::Entity e = chunk.entity_at(r);
                const std::uint64_t id = editor_id_of(world, e);
                rows.push_back({id == kNoEditorId ? ~std::uint64_t{0} : id, rows.size(), e});
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.key != b.key ? a.key < b.key : a.position < b.position;
    });
    std::vector<ecs::Entity> out;
    out.reserve(rows.size());
    for (const Row& r : rows) {
        out.push_back(r.e);
    }
    return out;
}

// ── EditorIds ───────────────────────────────────────────────────────────────────────────────────

std::size_t EditorIds::assign_missing(ecs::World& world) {
    register_editor_components(world);
    // Collect first, mutate after: adding EditorId moves an entity to another archetype, which
    // would skip or revisit rows mid-walk (the collect-then-mutate rule despawn_all also follows).
    std::vector<ecs::Entity> missing;
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t r = 0; r < chunk.size(); ++r) {
                const ecs::Entity e = chunk.entity_at(r);
                const EditorId* id = world.get<EditorId>(e);
                if (id == nullptr || id->value == kNoEditorId) {
                    missing.push_back(e);
                } else if (id->value != kDanglingEditorId && id->value >= next_) {
                    next_ = id->value + 1; // never issue an id something already carries
                }
            }
        }
    }
    // Slot-index order. On a world the scene loader just filled, slots were handed out 0, 1, 2… in
    // record order, so this is file order — deterministic, and independent of archetype layout.
    std::sort(missing.begin(), missing.end(), [](ecs::Entity a, ecs::Entity b) {
        return a.index < b.index;
    });
    for (const ecs::Entity e : missing) {
        (void)assign(world, e);
    }
    return missing.size();
}

std::uint64_t EditorIds::assign(ecs::World& world, ecs::Entity e) {
    register_editor_components(world);
    const std::uint64_t id = next_++;
    adopt(world, e, id);
    return id;
}

bool EditorIds::can_adopt(ecs::World& world, std::uint64_t id) {
    if (id == kNoEditorId || id == kDanglingEditorId || id >= next_) {
        ++counters_.refused_adoptions;
        RIME_ERROR("editorhost: refusing to respawn under id {} — this session never issued it",
                   id);
        return false;
    }
    if (resolve(world, id) != ecs::kNullEntity) {
        ++counters_.refused_adoptions;
        RIME_ERROR("editorhost: refusing to respawn under id {} — a live entity already holds it",
                   id);
        return false;
    }
    return true;
}

void EditorIds::adopt(ecs::World& world, ecs::Entity e, std::uint64_t id) {
    register_editor_components(world);
    if (EditorId* existing = world.get<EditorId>(e)) {
        existing->value = id;
    } else {
        (void)world.add_component(e, EditorId{id});
    }
    world.mark_changed<EditorId>(e);
    cache_[id] = e;
}

ecs::Entity EditorIds::resolve(const ecs::World& world, std::uint64_t id) {
    if (id == kNoEditorId || id == kDanglingEditorId || !world.is_registered<EditorId>()) {
        return ecs::kNullEntity;
    }
    const auto check = [&](ecs::Entity e) { return editor_id_of(world, e) == id; };
    if (const auto it = cache_.find(id); it != cache_.end() && check(it->second)) {
        return it->second;
    }
    rebuild_cache(world);
    const auto it = cache_.find(id);
    return it != cache_.end() ? it->second : ecs::kNullEntity;
}

void EditorIds::rebuild_cache(const ecs::World& world) {
    cache_.clear();
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t r = 0; r < chunk.size(); ++r) {
                const ecs::Entity e = chunk.entity_at(r);
                const std::uint64_t id = editor_id_of(world, e);
                if (id != kNoEditorId) {
                    cache_[id] = e;
                }
            }
        }
    }
}

// ── Wire payloads ───────────────────────────────────────────────────────────────────────────────

namespace {

void write_blob(core::ByteWriter& w, std::span<const std::byte> blob) {
    w.u32(static_cast<std::uint32_t>(blob.size()));
    w.bytes(blob);
}

bool read_blob(core::ByteReader& r, std::vector<std::byte>& out) {
    std::uint32_t len = 0;
    std::span<const std::byte> bytes;
    if (!r.u32(len) || !r.bytes(bytes, len)) {
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return true;
}

} // namespace

std::vector<std::byte> serialize_set_component(const SetComponentMsg& msg) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u64(msg.editor_id);
    w.u64(msg.type_hash);
    write_blob(w, msg.blob);
    return out;
}

bool parse_set_component(std::span<const std::byte> payload, SetComponentMsg& out) {
    core::ByteReader r(payload);
    return r.u64(out.editor_id) && r.u64(out.type_hash) && read_blob(r, out.blob);
}

std::vector<std::byte> serialize_component_ref(const ComponentRefMsg& msg) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u64(msg.editor_id);
    w.u64(msg.type_hash);
    return out;
}

bool parse_component_ref(std::span<const std::byte> payload, ComponentRefMsg& out) {
    core::ByteReader r(payload);
    return r.u64(out.editor_id) && r.u64(out.type_hash);
}

std::vector<std::byte> serialize_entity_ref(std::uint64_t editor_id) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u64(editor_id);
    return out;
}

bool parse_entity_ref(std::span<const std::byte> payload, std::uint64_t& editor_id) {
    core::ByteReader r(payload);
    return r.u64(editor_id);
}

std::vector<std::byte> serialize_spawn_entity(const SpawnEntityMsg& msg) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u64(msg.editor_id);
    w.u8(msg.exact ? 1u : 0u);
    w.u16(static_cast<std::uint16_t>(msg.components.size()));
    for (const auto& [hash, blob] : msg.components) {
        w.u64(hash);
        write_blob(w, blob);
    }
    return out;
}

bool parse_spawn_entity(std::span<const std::byte> payload, SpawnEntityMsg& out) {
    core::ByteReader r(payload);
    std::uint8_t exact = 0;
    std::uint16_t count = 0;
    if (!r.u64(out.editor_id) || !r.u8(exact) || !r.u16(count)) {
        return false;
    }
    out.exact = exact != 0;
    out.components.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint64_t hash = 0;
        std::vector<std::byte> blob;
        if (!r.u64(hash) || !read_blob(r, blob)) {
            return false;
        }
        out.components.emplace_back(hash, std::move(blob));
    }
    return true;
}

std::vector<std::byte> serialize_edit_result(const EditResultMsg& msg) {
    std::vector<std::byte> out;
    core::ByteWriter w(out);
    w.u8(msg.ok ? 1u : 0u);
    w.u64(msg.editor_id);
    return out;
}

bool parse_edit_result(std::span<const std::byte> payload, EditResultMsg& out) {
    core::ByteReader r(payload);
    std::uint8_t ok = 0;
    if (!r.u8(ok) || !r.u64(out.editor_id)) {
        return false;
    }
    out.ok = ok != 0;
    return true;
}

} // namespace rime::editorhost
