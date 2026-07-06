// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.7 (minimal reflection). DESCRIBE: reflect<T>() reports the right field names,
// types, and offsets, including a nested reflected struct. SERIALIZE: a struct round-trips
// through a packed byte stream unchanged, the stream is the expected packed size, truncated input
// is rejected, and the reflection-driven debug dump names every field. Design:
// docs/design/reflection.md.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

#include "rime/core/reflect.hpp"

// Reflected test types live in a named namespace (offsetof needs standard-layout types; a named
// namespace keeps the ReflectionTraits specialization clean).
namespace rt {
struct Inner {
    std::int32_t id;
    float weight;
};

struct Outer {
    bool active;
    std::int32_t count;
    double value;
    Inner inner; // nested reflected struct
    std::uint64_t flags;
};
} // namespace rt

// Inner must be registered before Outer (which references it).
RIME_REFLECT_BEGIN(rt::Inner)
RIME_REFLECT_FIELD(id)
RIME_REFLECT_FIELD(weight)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rt::Outer)
RIME_REFLECT_FIELD(active)
RIME_REFLECT_FIELD(count)
RIME_REFLECT_FIELD(value)
RIME_REFLECT_FIELD(inner)
RIME_REFLECT_FIELD(flags)
RIME_REFLECT_END()

using namespace rime::core;

TEST_CASE("reflect() describes fields: names, types, offsets") {
    const TypeInfo& info = reflect<rt::Outer>();
    CHECK(std::string(info.name) == "rt::Outer");
    CHECK(info.size == sizeof(rt::Outer));
    REQUIRE(info.fields.size() == 5);

    CHECK(std::string(info.fields[0].name) == "active");
    CHECK(info.fields[0].type == FieldType::Bool);
    CHECK(info.fields[0].offset == offsetof(rt::Outer, active));

    CHECK(info.fields[1].type == FieldType::Int32);
    CHECK(info.fields[1].offset == offsetof(rt::Outer, count));

    CHECK(info.fields[2].type == FieldType::Double);
    CHECK(info.fields[2].offset == offsetof(rt::Outer, value));

    CHECK(info.fields[4].type == FieldType::UInt64);
    CHECK(info.fields[4].offset == offsetof(rt::Outer, flags));
}

TEST_CASE("a nested reflected struct is described recursively") {
    const TypeInfo& info = reflect<rt::Outer>();
    const Field& inner = info.fields[3];
    CHECK(std::string(inner.name) == "inner");
    CHECK(inner.type == FieldType::Struct);
    REQUIRE(inner.struct_type != nullptr);
    CHECK(inner.struct_type == &reflect<rt::Inner>()); // points at Inner's descriptor
    CHECK(inner.struct_type->fields.size() == 2);
    CHECK(std::string(inner.struct_type->fields[1].name) == "weight");
    CHECK(inner.struct_type->fields[1].type == FieldType::Float);
}

TEST_CASE("is_reflected distinguishes registered from unregistered types") {
    CHECK(is_reflected_v<rt::Outer>);
    CHECK(is_reflected_v<rt::Inner>);
    CHECK_FALSE(is_reflected_v<int>);
    CHECK_FALSE(is_reflected_v<float>);
}

TEST_CASE("serialize produces a packed stream and round-trips unchanged") {
    rt::Outer original{};
    original.active = true;
    original.count = -42;
    original.value = 3.1415926535;
    original.inner = rt::Inner{7, 0.25f};
    original.flags = 0xDEADBEEFCAFEull;

    const std::vector<std::byte> bytes = serialize(original);
    // Packed size = bool(1) + int32(4) + double(8) + Inner(int32 4 + float 4) + uint64(8) = 29.
    CHECK(bytes.size() == 1 + 4 + 8 + (4 + 4) + 8);

    rt::Outer restored{};
    REQUIRE(deserialize(restored, bytes));
    CHECK(restored.active == original.active);
    CHECK(restored.count == original.count);
    CHECK(restored.value == original.value); // exact: bit-for-bit memcpy round-trip
    CHECK(restored.inner.id == original.inner.id);
    CHECK(restored.inner.weight == original.inner.weight);
    CHECK(restored.flags == original.flags);
}

TEST_CASE("deserialize rejects a truncated stream") {
    rt::Inner original{99, 1.5f};
    std::vector<std::byte> bytes = serialize(original); // 8 bytes
    bytes.pop_back();                                   // corrupt: now too short
    rt::Inner restored{};
    CHECK_FALSE(deserialize(restored, bytes));
}

TEST_CASE("to_debug_string names every field, recursing into nested structs") {
    rt::Outer obj{};
    obj.active = true;
    obj.count = 7;
    obj.inner = rt::Inner{3, 0.5f};
    obj.flags = 9;
    const std::string dump = to_debug_string(obj);

    CHECK(dump.find("rt::Outer {") != std::string::npos);
    CHECK(dump.find("active: true") != std::string::npos);
    CHECK(dump.find("count: 7") != std::string::npos);
    CHECK(dump.find("inner: rt::Inner {") != std::string::npos); // nested type dumped recursively
    CHECK(dump.find("id: 3") != std::string::npos);
    CHECK(dump.find("flags: 9") != std::string::npos);
}

// ── type_hash: the schema fingerprint cooked data (ADR-0024) versions against ────────────────────
// Types whose serialized shape differs in each way a cooked file must detect: a reorder, a rename,
// a retype, and a change buried in a nested struct.
namespace rt {
struct InnerClone { // same field names/types/order as Inner => same fingerprint
    std::int32_t id;
    float weight;
};

struct Reordered { // Inner's two fields, swapped
    float weight;
    std::int32_t id;
};

struct Renamed { // Inner with a member renamed
    std::int32_t identifier;
    float weight;
};

struct Retyped { // Inner's names/order, but id widened to 64-bit
    std::int64_t id;
    float weight;
};

struct HoldsInner {
    Inner nested;
};

struct HoldsReordered { // same field name/kind as HoldsInner, but the nested layout differs
    Reordered nested;
};
} // namespace rt

RIME_REFLECT_BEGIN(rt::InnerClone)
RIME_REFLECT_FIELD(id)
RIME_REFLECT_FIELD(weight)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rt::Reordered)
RIME_REFLECT_FIELD(weight)
RIME_REFLECT_FIELD(id)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rt::Renamed)
RIME_REFLECT_FIELD(identifier)
RIME_REFLECT_FIELD(weight)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rt::Retyped)
RIME_REFLECT_FIELD(id)
RIME_REFLECT_FIELD(weight)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rt::HoldsInner)
RIME_REFLECT_FIELD(nested)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rt::HoldsReordered)
RIME_REFLECT_FIELD(nested)
RIME_REFLECT_END()

TEST_CASE("type_hash is layout-based, not name-based: identical shapes hash equal") {
    CHECK(reflect<rt::Inner>().type_hash != 0);
    CHECK(reflect<rt::InnerClone>().type_hash == reflect<rt::Inner>().type_hash);
}

TEST_CASE("type_hash flips on reorder, rename, or retype") {
    const std::uint64_t base = reflect<rt::Inner>().type_hash;
    CHECK(reflect<rt::Reordered>().type_hash != base);
    CHECK(reflect<rt::Renamed>().type_hash != base);
    CHECK(reflect<rt::Retyped>().type_hash != base);
}

TEST_CASE("type_hash propagates a change in a nested struct's layout") {
    // Same outer field (name + Struct kind); only the nested type's layout differs — yet the outer
    // fingerprints must diverge, or a cooked file could not detect a nested-layout change.
    CHECK(reflect<rt::HoldsInner>().type_hash != reflect<rt::HoldsReordered>().type_hash);
}

TEST_CASE("type_hash is stable across calls") {
    CHECK(reflect<rt::Outer>().type_hash == reflect<rt::Outer>().type_hash);
}
