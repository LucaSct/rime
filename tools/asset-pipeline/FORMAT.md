# RMA1 cooked format — what this crate writes

This is the authoritative byte layout the cooker emits and the engine reads. It mirrors
[ADR-0024](../../docs/adr/0024-asset-model.md) §3 and [docs/design/assets.md](../../docs/design/assets.md);
the C++ reader is `engine/assets/src/cooked_reader.cpp`. Keep all three in lockstep — the cross-language
fixture test (`tests/cook_fixture.rs` cooking, `tests/assets/fixture_test.cpp` reading) is the alarm if
they drift.

All integers are **little-endian**, written **field by field** (never a struct copy).

## Container header (24 bytes)

| field | type | value |
| --- | --- | --- |
| `magic` | 4 bytes | `"RMA1"` (literal) |
| `container_version` | u16 | `1` |
| `asset_kind` | u16 | `1` = Mesh (append, never renumber) |
| `type_schema_hash` | u64 | mesh: `0x198738A2DDE250AC` (see below) |
| `payload_size` | u64 | length of the payload that follows |

The **asset id** is the FNV-1a 64 hash of the payload bytes (identical to the engine's `content_hash`).

## Mesh payload

| field | type | notes |
| --- | --- | --- |
| `vertex_attribs` | u32 | bitfield: position(1) \| normal(2) \| uv(4); v1 = `7` |
| `vertex_stride` | u32 | `32` for v1 (position + normal + uv, all f32) |
| `vertex_count` | u32 | |
| `index_count` | u32 | multiple of 3 (triangle list) |
| `aabb_min` | 3 × f32 | local-space bounds over the (world-flattened) positions |
| `aabb_max` | 3 × f32 | |
| `submesh_count` | u32 | one per source primitive |
| submeshes | `submesh_count` × (u32 first_index, u32 index_count, u32 material_slot) | material_slot = 0 until M6.4 |
| vertices | `vertex_count` × 32 bytes | interleaved `px,py,pz, nx,ny,nz, u,v` (f32) |
| indices | `index_count` × u32 | |

## The schema hash

`type_schema_hash` is the engine's reflection `type_hash` of the v1 vertex layout, computed and pinned
in C++ (`engine/assets`, `mesh_schema_hash()`), and re-declared here as `cooked::MESH_SCHEMA_HASH`. The
reader rejects a mismatch with a "re-cook" error, so a change to the vertex layout forces both sides to
update together. If you change the layout, update the constant in both languages and regenerate the
fixtures.
