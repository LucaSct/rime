// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

// Minimal compile-time-registered reflection: a way to ask, at runtime, "what fields does this
// struct have — their names, types, and byte offsets?" That single capability powers generic
// tools that would otherwise need hand-written per-type code: binary serialization (M1.7's proof),
// a debug-string dumper, and later the editor's property inspectors (M9) and asset (de)serializers
// (M6). The engine deliberately keeps this small and explicit — you opt a struct in with a short
// macro — rather than reaching for heavyweight reflection libraries; it is enough to *describe and
// serialize a struct*, and it reads clearly. Design: docs/design/reflection.md.
namespace rime::core {

// The primitive field kinds we can describe, plus Struct for a nested reflected type. Integers are
// classified by width+signedness (see make_field), so `int`/`long`/`std::int32_t` all just work.
enum class FieldType {
    Bool,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    Struct, // a nested reflected type; see Field::struct_type
};

struct TypeInfo; // a struct's full description (below)

// One member of a reflected struct.
struct Field {
    const char* name;                      // the member's source name
    std::size_t offset;                    // byte offset within the struct (from offsetof)
    FieldType type;                        // which kind
    const TypeInfo* struct_type = nullptr; // non-null iff type == Struct
};

// The description of a reflected struct: its name, size, and ordered fields.
struct TypeInfo {
    const char* name;
    std::size_t size;
    std::vector<Field> fields;
};

// Customization point specialized by the RIME_REFLECT macros (below), one per reflected type.
// Left undefined for un-reflected types, which is what the is_reflected trait detects.
template <class T> struct ReflectionTraits;

template <class> inline constexpr bool always_false = false;

// True iff T has been registered with RIME_REFLECT (i.e. ReflectionTraits<T> is complete).
template <class T, class = void> struct is_reflected : std::false_type {};

template <class T>
struct is_reflected<T, std::void_t<decltype(ReflectionTraits<T>::info())>> : std::true_type {};
template <class T> inline constexpr bool is_reflected_v = is_reflected<T>::value;

// The public accessor: the TypeInfo for a reflected type T.
template <class T> [[nodiscard]] const TypeInfo& reflect() {
    return ReflectionTraits<T>::info();
}

// Build a Field descriptor for a member of (deduced) C++ type FieldT. Integers map by width and
// signedness; a nested reflected struct becomes FieldType::Struct carrying its TypeInfo; anything
// else is a compile error naming the offending type.
template <class FieldT> [[nodiscard]] Field make_field(const char* name, std::size_t offset) {
    Field f;
    f.name = name;
    f.offset = offset;
    if constexpr (std::is_same_v<FieldT, bool>) {
        f.type = FieldType::Bool;
    } else if constexpr (std::is_same_v<FieldT, float>) {
        f.type = FieldType::Float;
    } else if constexpr (std::is_same_v<FieldT, double>) {
        f.type = FieldType::Double;
    } else if constexpr (std::is_integral_v<FieldT>) {
        // Classify by size so int / unsigned / long / fixed-width all resolve consistently.
        if constexpr (sizeof(FieldT) == 4) {
            f.type = std::is_signed_v<FieldT> ? FieldType::Int32 : FieldType::UInt32;
        } else if constexpr (sizeof(FieldT) == 8) {
            f.type = std::is_signed_v<FieldT> ? FieldType::Int64 : FieldType::UInt64;
        } else {
            static_assert(always_false<FieldT>,
                          "reflection supports 4- and 8-byte integers (and bool)");
        }
    } else if constexpr (is_reflected_v<FieldT>) {
        f.type = FieldType::Struct;
        f.struct_type = &reflect<FieldT>();
    } else {
        static_assert(always_false<FieldT>, "field type is not reflectable (reflect it first?)");
    }
    return f;
}

// Number of serialized bytes for a primitive field kind (Struct is handled by recursion).
[[nodiscard]] inline std::size_t primitive_size(FieldType type) noexcept {
    switch (type) {
        case FieldType::Bool:
            return 1;
        case FieldType::Int32:
        case FieldType::UInt32:
        case FieldType::Float:
            return 4;
        case FieldType::Int64:
        case FieldType::UInt64:
        case FieldType::Double:
            return 8;
        case FieldType::Struct:
            return 0; // not a primitive; serialized by walking the nested fields
    }
    return 0;
}

} // namespace rime::core

// ---- Registration macros ------------------------------------------------------------------
// Opt a struct into reflection right after its definition, at global scope:
//
//     struct Foo { int a; float b; Bar nested; };   // Bar must already be reflected
//     RIME_REFLECT_BEGIN(Foo)
//         RIME_REFLECT_FIELD(a)
//         RIME_REFLECT_FIELD(b)
//         RIME_REFLECT_FIELD(nested)
//     RIME_REFLECT_END()
//
// The macros specialize ReflectionTraits<Foo> with a lazily-built, function-local-static TypeInfo
// (so registration is thread-safe and pay-once). Reflected types must be standard-layout, since we
// use offsetof. A nested reflected struct must be registered before the type that contains it.
#define RIME_REFLECT_BEGIN(TYPE)                                                                   \
    namespace rime::core {                                                                         \
    template <> struct ReflectionTraits<TYPE> {                                                    \
        static const TypeInfo& info() {                                                            \
            using T = TYPE;                                                                        \
            static const TypeInfo s_info = [] {                                                     \
                TypeInfo t;                                                                          \
                t.name = #TYPE;                                                                      \
                t.size = sizeof(T);

#define RIME_REFLECT_FIELD(NAME)                                                                   \
    t.fields.push_back(make_field<decltype(T::NAME)>(#NAME, offsetof(T, NAME)));

#define RIME_REFLECT_END()                                                                         \
    return t;                                                                                      \
    }                                                                                              \
    ();                                                                                            \
    return s_info;                                                                                 \
    }                                                                                              \
    }                                                                                              \
    ;                                                                                              \
    } /* namespace rime::core */
