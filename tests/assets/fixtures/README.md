# Asset test fixtures

Small, self-contained assets used by the M6 asset-pipeline and `engine/assets` tests.

- `quad.gltf` — a two-triangle quad (4 vertices, position/normal/uv, `u16` indices) whose single node
  is translated by `(1, 2, 3)`, so cooking it exercises transform flattening.
- `quad_flat.gltf` — the same quad with **no** `NORMAL` accessor, to exercise the cooker's flat-normal
  derivation.
- `quad.rmesh` — `quad.gltf` **cooked** to RMA1 by `tools/asset-pipeline`. This is the cross-language
  proof artifact: `tools/asset-pipeline/tests/cook_fixture.rs` asserts the cooker still produces these
  bytes, and `tests/assets/fixture_test.cpp` asserts the C++ reader ingests them.

These glTF files are **hand-authored for Rime** (generated from a small script, embedded buffers as
base64), not vendored from any external collection — so they carry no third-party license; they are
covered by the repository's Apache-2.0.

To regenerate the cooked fixture after an intentional format change:

```
cargo run --manifest-path tools/Cargo.toml --bin rime -- \
    cook tests/assets/fixtures/quad.gltf --out tests/assets/fixtures
```
