# Asset loading — the RMA1 cooked container (design note)

This note documents the runtime asset boundary implemented by `engine/assets` (M6.1). It is the
concrete companion to [ADR-0024](../adr/0024-asset-model.md), which decided the model; here we pin the
byte layout, the reading discipline, and the seams left for later bricks. The offline cooker that
*writes* these files is Rust (`tools/asset-pipeline`, M6.2+); this note is the format both sides obey.

## Why a cook step at all

The engine loads **only cooked files** — never a `.gltf`, `.png`, or `.stl`. Importing an interchange
format is a parsing problem with a large, bug-prone surface, and it forfeits the offline work (mip
generation, tangents, vertex de-dup, fracture, SDFs) that cooking exists to do. So Rust imports and
cooks offline, C++ loads at runtime, and **files are the boundary** (ADR-0001). The cost we accept is
that you cannot point the engine at a source file; the payoff is a tiny, exhaustively testable runtime
reader and a data layout we own.

## The container: `RMA1`

Every cooked file is a fixed, versioned header followed by one kind-specific payload:

```
offset  size  field
0       4     magic            = 'R','M','A','1'  (literal bytes; greppable in a hex dump)
4       2     container_version  u16, LE           = 1
6       2     asset_kind         u16, LE           (Mesh = 1; Texture/Material/… reserved)
8       8     type_schema_hash   u64, LE           (see "Schema versioning")
16      8     payload_size       u64, LE           (bytes of payload that follow)
24      …     payload            payload_size bytes
```

Header size is **24 bytes**. One asset per file in v1; packs/bundles are a later, format-compatible
layer because the header already stands alone. All integers are **little-endian and written
field-by-field** (never a struct memcpy), so a file is byte-identical on every compiler and CPU — the
same rule the S0.4 stream protocol set. The reader shares core's little-endian byte cursors
([`core/byte_cursor.hpp`](../../engine/core/include/rime/core/byte_cursor.hpp)).

### The mesh payload (`asset_kind = Mesh`)

```
u32   vertex_attribs        bitfield: Position|Normal|Uv|Tangent|Joints|Weights
u32   vertex_stride         bytes per vertex (must equal the sum of the enabled attributes)
u32   vertex_count
u32   index_count           a 32-bit triangle-list index buffer; must be a multiple of 3
f32×3 aabb_min              local-space bounds, computed at cook time
f32×3 aabb_max
u32   submesh_count         v1: 0 or 1
      submesh_count × { u32 first_index, u32 index_count, u32 material_slot }
byte  vertices[vertex_count × vertex_stride]     interleaved vertex blob
u32   indices[index_count]
```

v1 cooks the position/normal/uv layout (`vertex_stride = 32`), the PBR-ready minimum. The
**attribute-flags** design is decision 6 of the ADR: tangents (M6.4) and skinning attributes (M6.7)
are new flag bits and a wider stride, not a new container version, and the reader validates the layout
it hands the renderer. The in-memory `MeshAsset` mirrors what `engine/render`'s mesh registry consumes,
so uploading is a memcpy-and-create — the renderer owns GPU residency (wired at M6.6); `assets` never
depends on `render`/`rhi`.

## Trust nothing you read

A cooked file arrives from disk exactly the way a message arrives from the network: possibly truncated,
possibly hostile. The reader therefore:

- checks the **magic** first, so a wrong-format file is rejected before any length is interpreted;
- **bounds-checks every read** against the bytes actually present, failing with a typed `AssetError`
  and advancing nothing (the byte cursors are transactional per field);
- computes the size of every variable-length region in **64-bit arithmetic and requires it to equal
  the bytes that remain *before* sizing any allocation** — so a crafted-huge `vertex_count` or
  `index_count` is rejected, never turned into a multi-gigabyte `resize`;
- validates semantics: attribute flags are known and include Position, stride matches the flags, the
  index count is whole triangles, every index is `< vertex_count`, every submesh range is inside the
  index buffer.

`tests/assets/cooked_mesh_test.cpp` drives one crafted file per failure mode — including truncation at
*every* byte length — and the suite is ASan/UBSan-clean. This is the same posture as the S0.4 protocol
decoder; the negative battery is the proof.

## Identity, de-duplication, and the manifest

An asset **is** its cooked bytes: `AssetId` is the 64-bit FNV-1a content hash of its payload
([`core/hash.hpp`](../../engine/core/include/rime/core/hash.hpp)). One mechanism buys three things: a
stable identity that survives a *source* rename (the cooked bytes don't change), the cook-cache key,
and load de-duplication — the `AssetRegistry` returns the same handle for a second load of the same
content and stores it once. A second, separate hash of the *source path* is what humans and the M9
browser look up by; it is not the identity. The cook also emits a plain-text **manifest** (one line
per asset: `source-path ⇥ kind ⇥ id-hex ⇥ cooked-file`) — derived data that can be regenerated and so
can never lie, unlike a hand-authored database.

## Schema versioning (reflection `type_hash`)

`type_schema_hash` guards against silently misreading data written for a different layout. For meshes
it is the reflection `type_hash` of the v1 vertex layout — a stable 64-bit fingerprint over a reflected
type's field names, types, and order ([`core/reflect/type_info.hpp`](../../engine/core/include/rime/core/reflect/type_info.hpp)).
Change the layout and the fingerprint changes, so old files are rejected with a clean "re-cook" error
instead of being misinterpreted. The golden value is pinned in the mesh test (`0x198738A2DDE250AC`) so
a change is deliberate and so the Rust cooker embeds the same constant; the M6.2 cross-language
golden-fixture test verifies cooker and reader agree. The same fingerprint later gates M9 inspector
compatibility and M11 replication schemas — one mechanism, three consumers.

## Determinism

Cooked output must be a pure function of its input bytes and the cooker version (ADR-0024, decision 7):
no timestamps in payloads, no hash-map iteration order leaking into output (sorted traversal), version
stamped in the manifest. That byte-stability is what M8's seeded fracture and M11's event replication
stand on. Cross-*machine* cook identity is not promised (float codegen may differ); cooked data is
built on one machine and shipped as data.

## Seams left open (not built in M6.1)

- **Hot reload.** The registry hands out generational handles, never raw pointers into relocatable
  storage. That indirection is exactly what "re-cook, swap the slot's target, bump its generation"
  needs, so hot reload can arrive later without an interface break. Nothing implements it yet.
- **Async loading.** Loads are synchronous in M6.1; M6.5 moves them onto the `JobSystem` with
  placeholder assets while a load is in flight. The reader is already device-free and re-entrant.
- **Packs / bundles.** One asset per file today; the standalone header admits a pack layer later.
- **Compressed textures (BCn), KTX2/DDS.** The format enum reserves values; adopted only when a
  measured GPU-memory need appears (VISION: measure before optimize).
- **A shared byte cursor for `engine/stream`.** The stream protocol predates `core/byte_cursor.hpp`
  and keeps its own private copy; it can adopt the core one later (a mechanical change).
