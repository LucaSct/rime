# Minimal reflection & serialization — design note (M1.7)

Companion to `engine/core/include/rime/core/reflect/`. Reflection is the ability to ask, at
runtime, *what does this struct look like?* — its fields, their types, and their byte offsets.
That one capability removes a whole category of hand-written, error-prone boilerplate.

## Why an engine wants reflection

Several systems need to walk an arbitrary struct generically:

- **Serialization** — save/load and asset cooking (M6) must turn structs into bytes and back.
- **Editor inspectors** (M9) — a property panel that shows/edits any component needs to enumerate
  its fields by name and type.
- **Debugging/tooling** — dumping a struct's values, diffing, network replication.

Without reflection each of these is written *per type*, by hand, and drifts out of sync the moment
someone adds a field. With reflection you write the walker **once** and every registered struct
gets it for free. M1.7's proof — describe a struct and round-trip it through bytes — is the
smallest thing that demonstrates the whole idea.

## The approach: explicit, opt-in registration

Rime's reflection is deliberately **small and explicit**. You opt a struct in with a short macro
right after its definition; there is no compiler plugin, no code generation step, no RTTI walk:

```cpp
struct Transform { Vec3 position; Quat rotation; float scale; };
RIME_REFLECT_BEGIN(Transform)
    RIME_REFLECT_FIELD(position)
    RIME_REFLECT_FIELD(rotation)
    RIME_REFLECT_FIELD(scale)
RIME_REFLECT_END()
```

This buys clarity (you can read exactly what is reflected and how) at the cost of a few lines of
registration — a trade that fits a codebase meant to be learned from. Heavier "automatic"
reflection (macros that parse, or external generators) is a possible future upgrade behind the
same `TypeInfo` interface.

### What the macro generates

`TypeInfo` is the description: a name, a size, and a vector of `Field { name, offset, type,
struct_type }`. The macro specializes a customization point `ReflectionTraits<T>` whose `info()`
returns a **function-local `static const TypeInfo`** — so the descriptor is built once, lazily, in
a thread-safe way (C++ guarantees on local statics), with no global-initialization-order hazard.
Field offsets come from `offsetof`, which is why **reflected types must be standard-layout** (plain
aggregates — exactly the data-oriented structs the engine favors anyway).

### Classifying field types

`make_field<FieldT>()` deduces each field's `FieldType` with `if constexpr`:

- `bool`, `float`, `double` map directly;
- **integers are classified by width and signedness** (`sizeof` + `is_signed`), so `int`,
  `unsigned`, `long`, and the fixed-width `std::int32_t`/`std::uint64_t` all resolve to the same
  kinds — no surprises from platform integer sizes;
- a field that is itself a **reflected struct** becomes `FieldType::Struct` carrying a pointer to
  its `TypeInfo` (detected by the `is_reflected` trait, a `void_t` check for a complete
  `ReflectionTraits<T>`);
- anything else is a **compile error** that names the offending type.

`is_reflected<T>` doubles as the public "is this type registered?" query.

## Serialization (the proof)

`serialize` walks the `TypeInfo` and appends each primitive's bytes in declared order, recursing
into nested structs; `deserialize` reverses it with a cursor and bounds checks (returning false on
a truncated stream). Because we visit fields **explicitly** rather than `memcpy`-ing the whole
struct, the byte stream is **packed — no padding** — and independent of the compiler's struct
layout. `to_debug_string` uses the same walk to print `Type { field: value, ... }`, recursing into
nested structs — the "describe" half of the proof, and a genuinely useful debug tool.

Round-tripping is bit-exact (primitives are `memcpy`-ed), so even floating-point fields compare
equal after a save/load.

## Deliberate limitations (labeled, per CLAUDE.md)

- **Supported field types:** `bool`, 4- and 8-byte integers, `float`, `double`, and nested
  reflected structs. No strings, containers, pointers, enums, or arrays yet — each needs a small,
  deliberate extension (variable-length encoding for strings/containers; an enum kind). The
  `TypeInfo`/`FieldType` model is built to grow into them.
- **Endianness:** the binary format is host-endian (little-endian targets). A real on-disk asset
  format will pin byte order and add a version/schema header; this brick is the mechanism, not the
  file format.
- **Registration is explicit and ordered:** a nested struct must be registered before the struct
  that contains it.
- **No schema versioning / no field rename tolerance** yet — that belongs with the asset format.

## Where this goes

`TypeInfo` is the contract the rest of the engine will lean on: the asset pipeline (M6) serializes
cooked data through it, and the Rust editor (M9) drives reflection-based inspectors across the FFI
boundary. Designing the descriptor now — even with a minimal serializer behind it — is another
"seam before features" investment. *Inspired by: Unreal's UPROPERTY reflection and the
descriptor-based reflection in talks by Stefan Reinalter and others.*
