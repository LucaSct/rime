# engine/assets — cooked-asset loading (the runtime side of the asset pipeline)

`rime::assets` is the **load** half of Rime's asset model: it opens cooked binary files, validates
them completely, and hands back typed, registry-owned assets. It is the runtime counterpart to the
offline Rust `tools/asset-pipeline` (M6.2+), which imports source formats (glTF, PNG/JPEG, STL) and
**cooks** them into these files. **Files are the boundary** ([ADR-0001](../../docs/adr/0001-language-and-module-boundaries.md),
[ADR-0024](../../docs/adr/0024-asset-model.md)): the engine contains no glTF/PNG/STL parser and never
will — all importing happens in the tools, all loading happens here.

In the layer cake it sits above `core`/`platform` and **below** the renderer: the renderer *consumes*
assets and owns GPU residency; `assets` produces CPU data and never depends on `render` or `rhi`. That
one-way dependency is what keeps the whole reader unit-testable with in-memory buffers and no device.

## Status (M6, brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| Brick | Provides | State |
| --- | --- | --- |
| M6.1 | **RMA1 reader** + **AssetRegistry** + **manifest reader**: cooked meshes load synchronously, content-addressed, trust-nothing | landed |
| M6.2 | the Rust cooker (`rime-cli cook`) + glTF mesh import → a golden fixture this reader ingests in CI | landed |
| M6.3 | **textures** (`AssetKind::Texture`, `TextureAsset`, full mip chains) → `read_texture`; the `checker.rtex` cross-language fixture | landed |
| M6.4 | materials + tangents (new `AssetKind`, same container) | planned |
| M6.5 | async loading on the `JobSystem` + placeholders | planned |

## The cooked container (RMA1)

Every cooked file starts with a fixed, versioned header, then a kind-specific payload:

```
[ magic "RMA1" : 4 bytes ][ container_version : u16 ][ asset_kind : u16 ]
[ type_schema_hash : u64 ][ payload_size : u64 ][ payload : payload_size bytes ]
```

Everything is **little-endian**, read **field-by-field** (never a struct memcpy), and every length is
**bounds-checked against the bytes actually present before anything is allocated from it**. A cooked
file is bytes off disk, so it is treated like bytes off the wire — the same "trust nothing" discipline
as the S0.4 stream protocol. Any inconsistency is a clean, typed `AssetError`, never undefined
behaviour; `tests/assets/cooked_mesh_test.cpp` and `cooked_texture_test.cpp` drive one crafted file per
failure mode (bad magic, wrong version/kind/schema, truncation at every byte length, size
disagreements; for meshes a broken vertex layout / out-of-range index / bad submesh, for textures an
unknown format / a mip table inconsistent with the base extent) and the suite is ASan/UBSan-clean.

## Identity, de-duplication, and the manifest

An asset **is** its cooked bytes: its `AssetId` is the 64-bit FNV-1a **content hash** of its payload
([`core/hash.hpp`](../core/include/rime/core/hash.hpp)). The `AssetRegistry` de-duplicates on it —
loading the same content twice coalesces to one stored asset and returns the same handle, which is
what makes a repeated load (the cook cache / duplicate-reference case) essentially free. The cook also
emits a plain-text **manifest** (one line per asset: `source-path ⇥ kind ⇥ id ⇥ cooked-file`), derived
data the runtime and the M9 asset browser read; `Manifest::parse` reads it here.

## Schema versioning

Cooked payloads embed a `type_schema_hash`. It is a reflection `type_hash`
([`core/reflect`](../core/include/rime/core/reflect/type_info.hpp)) of the v1 layout record — the vertex
layout for a mesh, the `{width,height,offset,size}` mip record for a texture — so if that layout ever
changes the loader rejects old files with a "re-cook" error instead of misreading them. The same
reflection fingerprint later gates M9 inspector compatibility and M11 replication schemas —
one mechanism, three consumers. Hot reload is a documented seam (the handle indirection is shaped for
it), not a feature in M6; see [docs/design/assets.md](../../docs/design/assets.md).

## Layout

```
include/rime/assets/    # public interface (core types only; no rhi/render, no source-format parsers)
  asset_id.hpp          #   AssetId, AssetKind, content/source hashing
  mesh_asset.hpp        #   MeshAsset: attribute flags + interleaved vertex blob + indices + AABB + submeshes
  texture_asset.hpp     #   TextureAsset: extent + colour-space format + mip table + concatenated pixels
  cooked_reader.hpp     #   the RMA1 container reader + AssetError + mesh_schema_hash() / texture_schema_hash()
  registry.hpp          #   AssetRegistry: handle-based ownership, content-addressed de-dup, sync load
  manifest.hpp          #   the cook-manifest reader
src/
  cooked_reader.cpp     #   header validation + trust-nothing mesh/texture decode (reuses core's byte cursors)
  registry.cpp          #   load_mesh(path) via platform file I/O; de-dup; the only file that touches platform
  manifest.cpp          #   manifest text parsing
```

Proofs (all **GPU-free**, so they run on every CI OS):
- `tests/assets/cooked_mesh_test.cpp` / `cooked_texture_test.cpp` — a valid file round-trips to exactly
  the written data with the right content id; the negative battery refuses every malformed file cleanly.
- `tests/assets/fixture_test.cpp` — the Rust-cooked `quad.rmesh` / `checker.rtex` load through this
  reader (the cross-language proof); the texture's 1×1 mip is the gamma-correct 188.
- `tests/assets/manifest_test.cpp` — well-formed manifests parse and look up; malformed lines are rejected.
- `tests/assets/registry_test.cpp` — content-addressed de-duplication, invalid-handle safety, and a
  real `load_mesh(path)` round-trip through a temporary file.
