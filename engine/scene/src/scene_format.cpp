// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/scene/scene_format.hpp"

#include <fmt/core.h>

#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/reflect.hpp" // reflects Entity — the entity-reference field detector below keys on it
#include "rime/ecs/world.hpp"

// The `.rscene` writer and reader. Both are reflection-driven: they walk a component's TypeInfo,
// never a concrete struct, so a newly reflected component saves and loads for free. The one thing
// they know beyond "a struct of fields" is entity references — a `Parent`'s target — which must be
// rewritten to scene-local ids on save and back to fresh runtime entities on load, so a scene is a
// self-contained, position-independent document. See docs/design/scene-format.md.
namespace rime::scene {
namespace {

// The reflected description of ecs::Entity. A component field whose nested struct IS this type is
// an entity reference (the only thing the format treats specially). Pointer identity is exact:
// reflect<T> returns the one function-local-static TypeInfo per type, so make_field<Entity> stamped
// this very address into the field's struct_type.
const core::TypeInfo* entity_type() {
    return &core::reflect<ecs::Entity>();
}

bool is_entity_field(const core::Field& f) {
    return f.type == core::FieldType::Struct && f.struct_type == entity_type();
}

// A stable 64-bit key for an Entity handle, so we can map handles → scene-local ids in a hash map.
std::uint64_t entity_key(ecs::Entity e) {
    return (static_cast<std::uint64_t>(e.index) << 32) | static_cast<std::uint64_t>(e.generation);
}

using LocalIdMap = std::unordered_map<std::uint64_t, std::uint32_t>; // entity_key → scene-local id

// Resolve a component's stable type_hash to this world's ComponentId, or kInvalidComponentId.
// Keying on the hash (not the registration-order id) is what lets a file load into a
// differently-ordered registry — the same contract as the m9.1 editor host.
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

// ── Writer ────────────────────────────────────────────────────────────────────────────────

void append_primitive(std::string& out, core::FieldType type, const std::byte* p) {
    auto it = std::back_inserter(out);
    switch (type) {
        case core::FieldType::Bool: {
            bool v = false;
            std::memcpy(&v, p, sizeof(v));
            out += v ? "true" : "false";
            break;
        }
        case core::FieldType::Int32: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(it, "{}", v);
            break;
        }
        case core::FieldType::UInt32: {
            std::uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(it, "{}", v);
            break;
        }
        case core::FieldType::Int64: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(it, "{}", v);
            break;
        }
        case core::FieldType::UInt64: {
            std::uint64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(it, "{}", v);
            break;
        }
        case core::FieldType::Float: {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            // fmt's default float format is the SHORTEST decimal that round-trips — the property
            // the reader's std::from_chars<float> relies on to recover the exact same bits.
            fmt::format_to(it, "{}", v);
            break;
        }
        case core::FieldType::Double: {
            double v = 0.0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(it, "{}", v);
            break;
        }
        case core::FieldType::Struct:
            break; // never a leaf; handled by the caller
    }
}

std::string entity_ref_text(ecs::Entity e, const LocalIdMap& to_local) {
    if (e == ecs::kNullEntity) {
        return "null";
    }
    const auto it = to_local.find(entity_key(e));
    if (it == to_local.end()) {
        return "null"; // a reference outside the saved set collapses to null (documented)
    }
    return "@" + std::to_string(it->second);
}

void indent(std::string& out, int level) {
    out.append(static_cast<std::size_t>(level) * 2, ' ');
}

void emit_field(std::string& out,
                const core::Field& f,
                const std::byte* base,
                int level,
                const LocalIdMap& to_local);

// Emit a struct's fields. All-primitive structs go on one line (`{ x 0 y 1 z 0 }`) — compact and
// the common case (a Vec3); a struct that nests another struct goes multi-line so deep placement
// stays diffable.
void emit_struct(std::string& out,
                 const core::TypeInfo& type,
                 const std::byte* base,
                 int level,
                 const LocalIdMap& to_local) {
    bool has_nested = false;
    for (const core::Field& f : type.fields) {
        if (f.type == core::FieldType::Struct) {
            has_nested = true;
            break;
        }
    }
    if (!has_nested) {
        out += "{ ";
        for (const core::Field& f : type.fields) {
            out += f.name;
            out += ' ';
            append_primitive(out, f.type, base + f.offset);
            out += ' ';
        }
        out += "}";
        return;
    }
    out += "{\n";
    for (const core::Field& f : type.fields) {
        emit_field(out, f, base, level + 1, to_local);
    }
    indent(out, level);
    out += "}";
}

void emit_field(std::string& out,
                const core::Field& f,
                const std::byte* base,
                int level,
                const LocalIdMap& to_local) {
    const std::byte* p = base + f.offset;
    indent(out, level);
    out += f.name;
    out += ' ';
    if (f.type == core::FieldType::Struct) {
        if (is_entity_field(f)) {
            ecs::Entity e;
            std::memcpy(&e, p, sizeof(e));
            out += entity_ref_text(e, to_local);
        } else {
            emit_struct(out, *f.struct_type, p, level, to_local);
        }
    } else {
        append_primitive(out, f.type, p);
    }
    out += '\n';
}

// One component: `  <type> 0x<hash> {` then every field on its own line. The component is always
// the multi-line unit (the thing you scan a scene for); nested structs inside it may still inline.
void emit_component(std::string& out,
                    const core::TypeInfo& type,
                    const std::byte* base,
                    int level,
                    const LocalIdMap& to_local) {
    indent(out, level);
    fmt::format_to(std::back_inserter(out), "{} 0x{:016x} {{\n", type.name, type.type_hash);
    for (const core::Field& f : type.fields) {
        emit_field(out, f, base, level + 1, to_local);
    }
    indent(out, level);
    out += "}\n";
}

// ── Reader: tokenizer ───────────────────────────────────────────────────────────────────────

constexpr bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Split the text into tokens: `{`, `}`, or a "word" (a maximal run of everything else). `#` starts
// a comment to end of line. Whitespace is insignificant, so the writer's indentation and a hand
// author's are equally valid. Tokens are views into `text`, which must outlive them.
std::vector<std::string_view> tokenize(std::string_view text) {
    std::vector<std::string_view> toks;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '#') {
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (is_space(c)) {
            ++i;
            continue;
        }
        if (c == '{' || c == '}') {
            toks.push_back(text.substr(i, 1));
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < text.size() && !is_space(text[i]) && text[i] != '{' && text[i] != '}' &&
               text[i] != '#') {
            ++i;
        }
        toks.push_back(text.substr(start, i - start));
    }
    return toks;
}

// Parse an unsigned/signed/float token in full (the whole token must be consumed, else it's a bad
// value). Locale-independent and round-trip-exact via std::from_chars.
template <class T> bool parse_number(std::string_view tok, T& out) {
    const char* first = tok.data();
    const char* last = tok.data() + tok.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

bool parse_hash(std::string_view tok, std::uint64_t& out) {
    if (tok.size() < 3 || tok[0] != '0' || (tok[1] != 'x' && tok[1] != 'X')) {
        return false;
    }
    const char* first = tok.data() + 2;
    const char* last = tok.data() + tok.size();
    const auto [ptr, ec] = std::from_chars(first, last, out, 16);
    return ec == std::errc{} && ptr == last;
}

bool parse_primitive_into(core::FieldType type, std::string_view tok, std::byte* p) {
    switch (type) {
        case core::FieldType::Bool: {
            bool v = false;
            if (tok == "true") {
                v = true;
            } else if (tok == "false") {
                v = false;
            } else {
                return false;
            }
            std::memcpy(p, &v, sizeof(v));
            return true;
        }
        case core::FieldType::Int32: {
            std::int32_t v = 0;
            if (!parse_number(tok, v)) {
                return false;
            }
            std::memcpy(p, &v, sizeof(v));
            return true;
        }
        case core::FieldType::UInt32: {
            std::uint32_t v = 0;
            if (!parse_number(tok, v)) {
                return false;
            }
            std::memcpy(p, &v, sizeof(v));
            return true;
        }
        case core::FieldType::Int64: {
            std::int64_t v = 0;
            if (!parse_number(tok, v)) {
                return false;
            }
            std::memcpy(p, &v, sizeof(v));
            return true;
        }
        case core::FieldType::UInt64: {
            std::uint64_t v = 0;
            if (!parse_number(tok, v)) {
                return false;
            }
            std::memcpy(p, &v, sizeof(v));
            return true;
        }
        case core::FieldType::Float: {
            float v = 0.0f;
            if (!parse_number(tok, v)) {
                return false;
            }
            std::memcpy(p, &v, sizeof(v));
            return true;
        }
        case core::FieldType::Double: {
            double v = 0.0;
            if (!parse_number(tok, v)) {
                return false;
            }
            std::memcpy(p, &v, sizeof(v));
            return true;
        }
        case core::FieldType::Struct:
            return false; // handled by the recursive struct filler, never here
    }
    return false;
}

const core::Field* find_field(const core::TypeInfo& type, std::string_view name) {
    for (const core::Field& f : type.fields) {
        if (name == f.name) {
            return &f;
        }
    }
    return nullptr;
}

// The struct filler, plus the entity-id remap. A single object threads the error string so any
// failure deep in a nested struct surfaces one clear message.
struct Reader {
    const std::vector<std::string_view>& toks;
    const std::unordered_map<std::uint32_t, ecs::Entity>* local_to_entity = nullptr;
    std::string error{};

    bool fail(std::string msg) {
        if (error.empty()) {
            error = std::move(msg);
        }
        return false;
    }

    // Fill the struct `type` at `base` from tokens in [begin, end) — a sequence of `name value`
    // pairs. Unknown field name, a malformed value, or a struct/entity shape mismatch is a clean
    // error. A field the file omits keeps its default (add_component_raw default-constructed the
    // slot), which is what lets a hand-authored file set only what it cares about.
    bool
    fill_struct(std::size_t begin, std::size_t end, const core::TypeInfo& type, std::byte* base) {
        std::size_t i = begin;
        while (i < end) {
            const std::string_view fname = toks[i++];
            const core::Field* f = find_field(type, fname);
            if (f == nullptr) {
                return fail(fmt::format("unknown field '{}' in '{}'", fname, type.name));
            }
            std::byte* p = base + f->offset;
            if (f->type == core::FieldType::Struct) {
                if (is_entity_field(*f)) {
                    if (i >= end) {
                        return fail(fmt::format("missing value for entity field '{}'", fname));
                    }
                    if (!write_entity_ref(toks[i++], p, fname)) {
                        return false;
                    }
                } else {
                    if (i >= end || toks[i] != "{") {
                        return fail(fmt::format("expected '{{' for struct field '{}'", fname));
                    }
                    ++i; // consume '{'
                    const std::size_t sub_begin = i;
                    std::size_t depth = 1;
                    while (i < end && depth > 0) {
                        if (toks[i] == "{") {
                            ++depth;
                        } else if (toks[i] == "}") {
                            if (--depth == 0) {
                                break;
                            }
                        }
                        ++i;
                    }
                    if (depth != 0) {
                        return fail(fmt::format("unterminated struct field '{}'", fname));
                    }
                    if (!fill_struct(sub_begin, i, *f->struct_type, p)) {
                        return false;
                    }
                    ++i; // consume '}'
                }
            } else {
                if (i >= end) {
                    return fail(fmt::format("missing value for field '{}'", fname));
                }
                if (!parse_primitive_into(f->type, toks[i], p)) {
                    return fail(fmt::format("bad value '{}' for field '{}'", toks[i], fname));
                }
                ++i;
            }
        }
        return true;
    }

    bool write_entity_ref(std::string_view tok, std::byte* p, std::string_view fname) {
        ecs::Entity e = ecs::kNullEntity;
        if (tok == "null") {
            e = ecs::kNullEntity;
        } else if (!tok.empty() && tok[0] == '@') {
            std::uint32_t local = 0;
            if (!parse_number(tok.substr(1), local)) {
                return fail(fmt::format("bad entity reference '{}' for field '{}'", tok, fname));
            }
            const auto it = local_to_entity->find(local);
            if (it == local_to_entity->end()) {
                return fail(
                    fmt::format("dangling entity reference '{}' for field '{}'", tok, fname));
            }
            e = it->second;
        } else {
            return fail(fmt::format(
                "expected '@id' or 'null' for entity field '{}', got '{}'", fname, tok));
        }
        std::memcpy(p, &e, sizeof(e));
        return true;
    }
};

} // namespace

std::string save_scene_to_string(const ecs::World& world) {
    const ecs::ComponentRegistry& registry = world.components();

    // Pass 1: walk the world in a stable order, numbering each entity 0..N-1 and mapping its handle
    // to that local id — so an entity-reference field can be written as a local id in pass 2.
    struct Row {
        const ecs::Archetype* arch;
        std::uint32_t chunk;
        std::uint32_t row;
    };

    std::vector<Row> rows;
    LocalIdMap to_local;
    for (std::size_t ai = 0; ai < world.archetype_count(); ++ai) {
        const ecs::Archetype& arch = world.archetype(ai);
        for (std::uint32_t ci = 0; ci < arch.chunk_count(); ++ci) {
            const ecs::Chunk& chunk = arch.chunk(ci);
            for (std::uint32_t r = 0; r < chunk.size(); ++r) {
                to_local[entity_key(chunk.entity_at(r))] = static_cast<std::uint32_t>(rows.size());
                rows.push_back({&arch, ci, r});
            }
        }
    }

    std::string out;
    out += "# Rime scene v1 (.rscene) — reflection-serialized world. Human-diffable; hand-edit or "
           "regenerate.\n";
    fmt::format_to(std::back_inserter(out), "rime_scene {}\n\n", kSceneFormatVersion);
    for (std::size_t li = 0; li < rows.size(); ++li) {
        const Row& rw = rows[li];
        fmt::format_to(std::back_inserter(out), "entity {} {{\n", li);
        const ecs::Chunk& chunk = rw.arch->chunk(rw.chunk);
        for (const ecs::ComponentId id : rw.arch->signature().ids()) {
            const ecs::ComponentInfo& info = registry.info(id);
            if (info.type_info == nullptr) {
                continue; // unreflected component: no inspectable state to author
            }
            const auto* comp = static_cast<const std::byte*>(chunk.component(id, rw.row));
            emit_component(out, *info.type_info, comp, 1, to_local);
        }
        out += "}\n\n";
    }
    return out;
}

LoadReport load_scene_from_string(ecs::World& world, std::string_view text) {
    LoadReport report;
    const std::vector<std::string_view> toks = tokenize(text);

    // Header: `rime_scene <version>`.
    std::size_t pos = 0;
    if (pos >= toks.size() || toks[pos] != "rime_scene") {
        report.error = "not a scene file: expected a 'rime_scene' header";
        return report;
    }
    ++pos;
    int version = 0;
    if (pos >= toks.size() || !parse_number(toks[pos], version)) {
        report.error = "scene header: expected a version number after 'rime_scene'";
        return report;
    }
    ++pos;
    if (version != kSceneFormatVersion) {
        report.error = fmt::format("unsupported scene format version {} (this build reads {})",
                                   version,
                                   kSceneFormatVersion);
        return report;
    }

    // A parsed component: its declared name + hash and the token range of its `{ ... }` body,
    // applied in phase 3 once every entity exists (so an entity reference can be remapped).
    struct ParsedComp {
        std::string_view name;
        std::uint64_t hash;
        std::size_t body_begin;
        std::size_t body_end;
    };

    struct ParsedEntity {
        std::uint32_t local_id;
        std::vector<ParsedComp> comps;
    };

    std::vector<ParsedEntity> entities;
    std::unordered_map<std::uint32_t, std::size_t> seen_local; // local id → entities index (dups)

    // Phase 1: structural parse. Validate shape and capture component bodies; interpret no values.
    while (pos < toks.size()) {
        if (toks[pos] != "entity") {
            report.error = fmt::format("expected 'entity' or end of file, got '{}'", toks[pos]);
            return report;
        }
        ++pos;
        std::uint32_t local_id = 0;
        if (pos >= toks.size() || !parse_number(toks[pos], local_id)) {
            report.error = "expected an entity id after 'entity'";
            return report;
        }
        ++pos;
        if (seen_local.count(local_id) != 0) {
            report.error = fmt::format("duplicate entity id {}", local_id);
            return report;
        }
        if (pos >= toks.size() || toks[pos] != "{") {
            report.error = fmt::format("expected '{{' after 'entity {}'", local_id);
            return report;
        }
        ++pos; // consume '{'

        ParsedEntity ent;
        ent.local_id = local_id;
        while (pos < toks.size() && toks[pos] != "}") {
            const std::string_view name = toks[pos];
            if (name == "{" || name == "}") {
                report.error = "expected a component type name";
                return report;
            }
            ++pos;
            std::uint64_t hash = 0;
            if (pos >= toks.size() || !parse_hash(toks[pos], hash)) {
                report.error = fmt::format("expected a 0x<hash> after component '{}'", name);
                return report;
            }
            ++pos;
            if (pos >= toks.size() || toks[pos] != "{") {
                report.error = fmt::format("expected '{{' for component '{}'", name);
                return report;
            }
            ++pos; // consume '{'
            const std::size_t body_begin = pos;
            std::size_t depth = 1;
            while (pos < toks.size() && depth > 0) {
                if (toks[pos] == "{") {
                    ++depth;
                } else if (toks[pos] == "}") {
                    if (--depth == 0) {
                        break;
                    }
                }
                ++pos;
            }
            if (depth != 0) {
                report.error = fmt::format("unterminated body for component '{}'", name);
                return report;
            }
            ent.comps.push_back({name, hash, body_begin, pos});
            ++pos; // consume the component's '}'
        }
        if (pos >= toks.size()) {
            report.error = fmt::format("unterminated entity {}", local_id);
            return report;
        }
        ++pos; // consume the entity's '}'
        seen_local.emplace(local_id, entities.size());
        entities.push_back(std::move(ent));
    }

    // Phase 2: spawn every entity first, so a reference to any of them (forward or backward)
    // resolves.
    std::vector<ecs::Entity> spawned(entities.size());
    std::unordered_map<std::uint32_t, ecs::Entity> local_to_entity;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        spawned[i] = world.spawn();
        local_to_entity.emplace(entities[i].local_id, spawned[i]);
    }
    report.entities = entities.size();

    // Phase 3: add + fill each component, remapping entity references through local_to_entity.
    const ecs::ComponentRegistry& registry = world.components();
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const ecs::Entity e = spawned[i];
        for (const ParsedComp& c : entities[i].comps) {
            const ecs::ComponentId id = id_for_type_hash(registry, c.hash);
            if (id == ecs::kInvalidComponentId) {
                // Distinguish "engine has this type but a DIFFERENT shape" (schema drift — the
                // stale-hash error the format is built to catch) from "never heard of it".
                for (std::size_t j = 0; j < registry.count(); ++j) {
                    const ecs::ComponentInfo& info =
                        registry.info(static_cast<ecs::ComponentId>(j));
                    if (info.type_info != nullptr && c.name == info.type_info->name) {
                        report.error = fmt::format(
                            "component '{}' schema drift: file hash 0x{:016x}, engine 0x{:016x} — "
                            "re-save the scene",
                            c.name,
                            c.hash,
                            info.type_info->type_hash);
                        return report;
                    }
                }
                report.error =
                    fmt::format("unknown component type '{}' (hash 0x{:016x}) — is it registered?",
                                c.name,
                                c.hash);
                return report;
            }
            const ecs::ComponentInfo& info = registry.info(id);
            std::byte* slot = static_cast<std::byte*>(world.add_component_raw(e, id));
            if (slot == nullptr) {
                report.error = fmt::format("could not add component '{}'", c.name);
                return report;
            }
            Reader rd{toks};
            rd.local_to_entity = &local_to_entity;
            if (!rd.fill_struct(c.body_begin, c.body_end, *info.type_info, slot)) {
                report.error = std::move(rd.error);
                return report;
            }
            ++report.components;
        }
    }

    report.ok = true;
    return report;
}

bool save_scene_file(const ecs::World& world, const std::filesystem::path& path) {
    const std::string text = save_scene_to_string(world);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

LoadReport load_scene_file(ecs::World& world, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return LoadReport{false, "cannot open scene file: " + path.string(), 0, 0};
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return load_scene_from_string(world, text);
}

} // namespace rime::scene
