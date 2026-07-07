# asset-pipeline — the offline (Rust) asset cooker

`asset-pipeline` is the **import + cook** half of Rime's asset model: it reads source art (glTF 2.0
meshes and PNG/JPEG textures today; materials and STL in later M6 bricks) and writes the engine's
runtime **RMA1** binary format. It is the *writer* of record for that format; the C++ `engine/assets`
module is the reader. **Files are the boundary** ([ADR-0001](../../docs/adr/0001-cpp-core-rust-tooling.md),
[ADR-0024](../../docs/adr/0024-asset-model.md)): this crate never links the engine, and the engine
never parses glTF or PNG.

Driven by [`rime-cli`](../rime-cli): `rime cook <input> --out <dir>` cooks a file or a directory
(`--srgb`/`--linear` picks a texture's colour space, sRGB by default); `rime inspect <file>` prints a
cooked file's header.

## Status (M6, brick by brick)

| Brick | Provides | State |
| --- | --- | --- |
| M6.2 | glTF **mesh** import → cook; `rime cook` / `rime inspect`; the cross-language fixture proof | landed |
| M6.3 | **texture** import (PNG/JPEG), offline gamma-correct mip chains, sRGB/linear | landed |
| M6.4 | metallic-roughness materials + tangent generation | planned |
| M6.6 | STL import (a second importer, proving the format is not glTF-shaped) | planned |

## What the mesh cooker does (M6.2)

1. **Import** the glTF with the [`gltf`](https://crates.io/crates/gltf) crate.
2. **Walk** the scene's node hierarchy and **flatten each node's world transform into its vertices**
   (positions by the matrix, normals by its inverse-transpose). v1 keeps no runtime scene graph from
   glTF; the transform is baked in. (Instancing is lost in v1 — an accepted trade, noted for M9 prefabs.)
3. **Extract** positions, normals, UVs, and indices per triangle primitive. `u8`/`u16` indices promote
   to `u32`; missing normals are derived from the geometry; missing UVs default to `(0,0)`.
4. **Merge** a file's primitives into one mesh with one submesh each, and **cook** it to an RMA1 file
   (`<stem>.rmesh`), plus a `manifest.txt`. Output is **deterministic**: sorted traversal, no
   timestamps — the same input bytes always cook to the same output bytes.

Out of scope for v1 (rejected with a clear message, or defaulted): non-triangle primitive modes,
Draco-compressed geometry, skins, materials/tangents (M6.4), and glTF extensions.

## What the texture cooker does (M6.3)

1. **Decode** a PNG or JPEG to RGBA8 with the [`image`](https://crates.io/crates/image) crate (PNG +
   JPEG features only). Rows stay top-first — no vertical flip — matching the engine's uv convention.
2. **Generate a full mip chain** offline, box-filtered down to 1×1. The filter runs in **linear
   light**: for an sRGB texture each texel is linearised, averaged, then re-encoded, so minified
   colour surfaces don't darken (the classic gamma-wrong-mips bug); a linear texture is averaged
   directly, and alpha is always linear. This is the one non-obvious technique here — see the module
   note in `texture.rs`.
3. **Cook** to an RMA1 texture file (`<stem>.rtex`) whose header, mip table, and pixels the engine
   uploads verbatim through `write_texture_mips` — no on-device chain regeneration.

The colour space is `--srgb` (default, for baseColor/emissive) or `--linear` (normal /
metallic-roughness / occlusion data). From M6.4 a material's usage will pick it automatically.

## Layout

```
src/lib.rs          # crate root: the PipelineError type + cook_gltf / cook_texture / cook_path orchestration
src/gltf_import.rs  # glTF → world-flattened Primitives (the only glTF-aware module)
src/mesh.rs         # the in-memory Mesh + the RMA1 mesh-payload encoder + flat-normal derivation
src/texture.rs      # image decode + gamma-correct offline mip generation + the RMA1 texture-payload encoder
src/cooked.rs       # the RMA1 container writer (magic/header) + FNV-1a content hash — writer of record
src/manifest.rs     # the deterministic cook-manifest writer
src/math.rs         # a tiny column-major Mat4 (transform flattening), dependency-free
FORMAT.md           # the RMA1 byte layout this crate writes (mirrors ADR-0024 / docs/design/assets.md)
```

## Proof

- `cargo test` — unit tests for the FNV hash (pinned to the same published vectors as the engine),
  the container round-trip, transform math (inverse-transpose normals), manifest formatting, and the
  **gamma-correct mip** (an sRGB checker's coarse mip is 188, not the too-dark 128); plus
  `tests/cook_fixture.rs`, which cooks the shared fixtures and asserts the bytes equal the committed
  `quad.rmesh` / `checker.rtex` (byte-stable, the drift alarm for the format).
- The engine's `tests/assets/fixture_test.cpp` loads those same files through the C++ reader — so the
  two languages are checked against one artifact each and cannot drift on the format silently.
