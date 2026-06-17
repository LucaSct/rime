// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "rime/core/reflect/type_info.hpp"

// Generic, reflection-driven (de)serialization and debug dumping. None of these functions know
// anything about a specific struct — they walk its TypeInfo. Add a struct to the reflection
// system (RIME_REFLECT) and it serializes for free. This is the M1.7 proof: describe a struct and
// round-trip it through bytes. Design/format notes: docs/design/reflection.md.
namespace rime::core {

// Serialize to a packed little-endian byte stream: each field's bytes in declared order, nested
// structs inlined recursively, no struct padding. (Endianness is the host's; we target
// little-endian platforms — a documented limitation until an asset format pins byte order.)
[[nodiscard]] std::vector<std::byte> serialize(const TypeInfo& type, const void* object);

// Inverse of serialize: fill `object` from a packed stream. Returns false (without completing) if
// `data` is shorter than the type requires. `object` must point at storage of the matching type.
[[nodiscard]] bool deserialize(const TypeInfo& type, void* object, std::span<const std::byte> data);

// Human-readable dump built from the field names + values, e.g.
//   Outer { active: true, count: 7, value: 1.5, inner: Inner { id: 3, weight: 0.25 }, flags: 9 }
[[nodiscard]] std::string to_debug_string(const TypeInfo& type, const void* object);

// Type-deduced conveniences for reflected types.
template <class T> [[nodiscard]] std::vector<std::byte> serialize(const T& object) {
    return serialize(reflect<T>(), &object);
}

template <class T> [[nodiscard]] bool deserialize(T& object, std::span<const std::byte> data) {
    return deserialize(reflect<T>(), &object, data);
}

template <class T> [[nodiscard]] std::string to_debug_string(const T& object) {
    return to_debug_string(reflect<T>(), &object);
}

} // namespace rime::core
