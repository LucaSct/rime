// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/reflect/serialize.hpp"

#include <fmt/core.h>

#include <cstring>
#include <iterator>

// Reflection-driven serialization. Each function recurses over a TypeInfo's fields; primitives are
// copied byte-for-byte and nested structs are walked. Because we visit fields explicitly (rather
// than memcpy-ing the whole struct), the output is packed with no padding and is stable regardless
// of the compiler's struct layout. See docs/design/reflection.md.
namespace rime::core {
namespace {

void serialize_into(const TypeInfo& type, const std::byte* base, std::vector<std::byte>& out) {
    for (const Field& field : type.fields) {
        const std::byte* p = base + field.offset;
        if (field.type == FieldType::Struct) {
            serialize_into(*field.struct_type, p, out);
        } else {
            out.insert(out.end(), p, p + primitive_size(field.type));
        }
    }
}

bool deserialize_from(const TypeInfo& type,
                      std::byte* base,
                      std::span<const std::byte> data,
                      std::size_t& cursor) {
    for (const Field& field : type.fields) {
        std::byte* p = base + field.offset;
        if (field.type == FieldType::Struct) {
            if (!deserialize_from(*field.struct_type, p, data, cursor)) {
                return false;
            }
        } else {
            const std::size_t n = primitive_size(field.type);
            if (cursor + n > data.size()) {
                return false; // truncated stream
            }
            std::memcpy(p, data.data() + cursor, n);
            cursor += n;
        }
    }
    return true;
}

// Read a primitive of the given kind from p and append its textual form to out.
void append_primitive(FieldType type, const std::byte* p, std::string& out) {
    auto out_it = std::back_inserter(out);
    switch (type) {
        case FieldType::Bool: {
            bool v = false;
            std::memcpy(&v, p, sizeof(v));
            out += v ? "true" : "false";
            break;
        }
        case FieldType::Int32: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(out_it, "{}", v);
            break;
        }
        case FieldType::UInt32: {
            std::uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(out_it, "{}", v);
            break;
        }
        case FieldType::Int64: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(out_it, "{}", v);
            break;
        }
        case FieldType::UInt64: {
            std::uint64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(out_it, "{}", v);
            break;
        }
        case FieldType::Float: {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(out_it, "{}", v);
            break;
        }
        case FieldType::Double: {
            double v = 0.0;
            std::memcpy(&v, p, sizeof(v));
            fmt::format_to(out_it, "{}", v);
            break;
        }
        case FieldType::Struct:
            break; // handled by the caller (recursion)
    }
}

void dump_into(const TypeInfo& type, const std::byte* base, std::string& out) {
    fmt::format_to(std::back_inserter(out), "{} {{ ", type.name);
    bool first = true;
    for (const Field& field : type.fields) {
        if (!first) {
            out += ", ";
        }
        first = false;
        fmt::format_to(std::back_inserter(out), "{}: ", field.name);
        const std::byte* p = base + field.offset;
        if (field.type == FieldType::Struct) {
            dump_into(*field.struct_type, p, out);
        } else {
            append_primitive(field.type, p, out);
        }
    }
    out += " }";
}

} // namespace

std::vector<std::byte> serialize(const TypeInfo& type, const void* object) {
    std::vector<std::byte> out;
    serialize_into(type, static_cast<const std::byte*>(object), out);
    return out;
}

bool deserialize(const TypeInfo& type, void* object, std::span<const std::byte> data) {
    std::size_t cursor = 0;
    return deserialize_from(type, static_cast<std::byte*>(object), data, cursor);
}

std::string to_debug_string(const TypeInfo& type, const void* object) {
    std::string out;
    dump_into(type, static_cast<const std::byte*>(object), out);
    return out;
}

} // namespace rime::core
