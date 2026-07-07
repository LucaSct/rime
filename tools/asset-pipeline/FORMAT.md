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
| `asset_kind` | u16 | `1` = Mesh, `2` = Texture, `3` = Material (append, never renumber) |
| `type_schema_hash` | u64 | mesh: `0x198738A2DDE250AC`; texture: `0xAB8A2B884141F736`; material: `0xCA4ED4CC434C941A` (see below) |
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

## Texture payload

| field | type | notes |
| --- | --- | --- |
| `width` | u32 | base level extent |
| `height` | u32 | |
| `format` | u32 | `0` = RGBA8 linear (UNORM), `1` = RGBA8 sRGB (BCn reserved) |
| `mip_count` | u32 | a full chain: `floor(log2(max(width,height))) + 1` |
| mip table | `mip_count` × (u32 width, u32 height, u32 offset, u32 size) | offsets tile the blob, no gap/overlap; `size = width·height·4` |
| pixels | `Σ size` bytes | every level's RGBA8 texels, level 0 first |

The chain is generated offline and **box-filtered in linear light** — for an sRGB texture the cooker
linearises, averages, then re-encodes (so minified colour doesn't darken); linear textures are averaged
directly, alpha always linear. sRGB is the `rime cook` default; `--linear` tags data textures. Rows are
top-first (no vertical flip: matches the engine's uv `v=0`-at-top convention). The engine uploads each
level verbatim via `write_texture_mips` — it never regenerates the chain.

## Material payload

A material is a **fixed** record — no variable-length tail — so the payload is exactly 92 bytes, read
straight through. Colours are linear (the cook converts sRGB authoring values).

| field | type | notes |
| --- | --- | --- |
| `base_color` | 4 × f32 | RGBA factor, multiplied with the base-color texture (glTF default `1,1,1,1`) |
| `emissive` | 3 × f32 | linear RGB emissive factor (glTF default `0,0,0`) |
| `metallic` | f32 | 0 = dielectric, 1 = metal (glTF default `1`) |
| `roughness` | f32 | 0 = mirror, 1 = rough (glTF default `1`) |
| `normal_scale` | f32 | scales the tangent-space normal XY (glTF default `1`) |
| `occlusion_strength` | f32 | lerps AO toward "no occlusion" (glTF default `1`) |
| `alpha_cutoff` | f32 | the Mask threshold (glTF default `0.5`) |
| `alpha_mode` | u32 | `0` = Opaque, `1` = Mask, `2` = Blend |
| `base_color_tex` | u64 | `AssetId` of the cooked texture (`0` = none → engine fallback); sRGB |
| `metallic_roughness_tex` | u64 | `0` = none; linear, G = roughness, B = metallic (glTF packing) |
| `normal_tex` | u64 | `0` = none; linear tangent-space normal |
| `occlusion_tex` | u64 | `0` = none; linear, R = ambient occlusion |
| `emissive_tex` | u64 | `0` = none; sRGB |

Texture slots are references, not pixels: a material names the `AssetId` (content hash) of an already-
cooked texture, and the engine resolves each to one shared GPU upload. The cook picks each texture's
colour space from its usage (base-color/emissive sRGB; normal/MR/occlusion linear). Materials are not
standalone source files — they are emitted from a glTF alongside its meshes.

## The schema hash

`type_schema_hash` is the engine's reflection `type_hash` of a v1 layout record, computed and pinned in
C++ and re-declared here. For a **mesh** it fingerprints the vertex layout (`mesh_schema_hash()` ↔
`cooked::MESH_SCHEMA_HASH`); for a **texture** it fingerprints the `{width, height, offset, size}` mip
record (`texture_schema_hash()` ↔ `cooked::TEXTURE_SCHEMA_HASH`); for a **material** it fingerprints the
whole fixed material record (`material_schema_hash()` ↔ `cooked::MATERIAL_SCHEMA_HASH`). The reader
rejects a mismatch with a "re-cook" error, so a layout change forces both sides to update together. If
you change a layout, update the constant in both languages and regenerate the fixtures.
