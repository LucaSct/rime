# ADR-0024: The asset model — identity, the cooked container, and the cook/load split (M6.0)

- Status: Accepted
- Date: 2026-07-06

## Context

M6 builds the asset pipeline: `tools/asset-pipeline` (Rust) imports source content (glTF,
textures, STL), **cooks** it into engine-ready binary files, and `engine/assets` (C++) loads
those at runtime. The milestone's "done when" is the full chain: *import → cook → load →
render a real glTF model with textures*.

Everything that ever references content flows through this model: scenes (M9), destructible
assets (M8), replication baselines (M11), the vision demo's content (M12). Asset identity and
versioning are classic hard-to-retrofit seams — an engine that ships cooked data it can no
longer validate, or identities that break on a rename, pays forever. So, in the M4.0/M5.0
pattern, this ADR settles the model before the first byte is written, and bricks M6.1–M6.10
cite it.

The forces:

- **ADR-0001 already fixed the language split and the boundary style:** Rust tools talk to
  the C++ engine across *stable* boundaries — files, CLI, or a C ABI — never internals. An
  asset pipeline is the files-as-boundary case par excellence.
- **The engine must trust nothing it reads.** Cooked files arrive from disk like network
  bytes arrive from the wire; the S0.4 protocol set the house discipline (versioned header,
  little-endian field-by-field, bounds-checked reads, clean typed failure).
- **Types evolve.** A cooked mesh written against last month's vertex layout must be
  *detected*, not misread. Reflection (M1.7, extended at M4.1) is the natural place to hang a
  schema fingerprint.
- **Determinism is about to become load-bearing.** M8 cooks seeded fracture into assets and
  M11 replicates destruction *events* against identical cooked data on every machine — so
  "same input ⇒ same cooked bytes" is a correctness property, not a nicety.
- **We run GPU-poor.** Dev/CI render on lavapipe; exotic texture formats buy nothing today.
  The texture policy should start plain and reserve room for compressed formats when real
  GPU-memory pressure is measured.

## Decision

Ten parts, each cited by the brick that implements it.

1. **Split of labor: Rust cooks, C++ loads, files are the boundary.** All parsing of source
   formats (glTF, PNG/JPEG, STL) happens offline in `tools/asset-pipeline`, driven by
   `rime-cli cook`. The engine's `engine/assets` reads **only** cooked files — it never
   contains a glTF or PNG parser. (M6.1, M6.2)

2. **Identity: content-hashed `AssetId`, plus a manifest for lookup.** An asset's identity
   is a 64-bit **content hash of its cooked payload**; a second 64-bit hash of its *source
   path* serves human-facing lookup. The cook emits a plain-text **manifest** (one line per
   asset: source path · kind · asset id · cooked filename) that the runtime registry, the
   M9 asset browser, and humans all read. Renaming a source re-keys its path hash but not
   its content identity; editing content re-cooks and re-keys. The hash is **FNV-1a 64**,
   implemented once in `core` with a short teaching note — not cryptographic, and that's
   fine: identity needs collision *rarity* plus the cook's ability to detect a collision
   across the manifest it just wrote (it sees every asset; a collision is a hard cook
   error). At engine-realistic asset counts (≤10⁵) the 64-bit birthday risk is ~10⁻⁹.

3. **The cooked container: `RMA1`, field-by-field, validation-first.** Every cooked file
   starts `[magic "RMA1":u32][container_version:u16][asset_kind:u16][type_schema_hash:u64]
   [payload_size:u64]` followed by a kind-specific payload — **little-endian, written and
   read field-by-field** (never a struct memcpy), bounds-checked against file size at every
   step, exactly the S0.4 protocol discipline (the reader reuses its `ByteReader` pattern).
   One asset per file in v1; packs/bundles are a later, format-compatible layer because the
   header already stands alone. (M6.1)

4. **Schema versioning: reflection grows a stable `type_hash`.** `RIME_REFLECT_*` gains a
   64-bit hash over a type's field names, field types, and order (FNV-1a again). Cooked
   payloads embed the hash of the layout they were cooked against; the loader rejects a
   mismatch with a clear "re-cook" error instead of misreading bytes. The same hashes later
   gate M9 inspector compatibility and M11 replication schemas — one fingerprint, three
   consumers. (M6.1; the reflection extension is its own small, tested change)

5. **Texture pixel policy v0: RGBA8 + offline mips.** Textures cook as RGBA8 (sRGB or UNORM
   chosen by *semantic*: baseColor/emissive are sRGB; normal/metallic-roughness/occlusion
   are linear), with the full mip chain generated **offline in linear space**. The format
   enum reserves values for BC1/3/5/7 — adopted only when a measured GPU-memory or
   bandwidth need appears (VISION: measure before optimize). (M6.3)

6. **Vertex layout: attribute flags from day one.** Cooked meshes carry an attribute-flags
   bitfield (position | normal | uv | tangent | joints | weights) plus per-attribute
   offsets/strides, so tangents (M6.4) and skinning attributes (M6.7) extend the format
   without a container break, and the loader validates the layout it hands the renderer.
   (M6.1, M6.2)

7. **Deterministic cooks: same input ⇒ same bytes.** No timestamps inside payloads, no
   hash-map iteration order leaking into output (sorted traversal everywhere), cooker
   version stamped in the manifest. CI asserts byte-stability by cooking twice and
   comparing. Cross-*machine* cook identity is not promised (float codegen may differ);
   cooked data is built on one machine and shipped as data — same-binary determinism is
   the contract M8/M11 need. (M6.2 onward, every cooker)

8. **The cook cache.** Cooked output lives out-of-tree (`cooked/` beside the source
   directory, or `--out`); a cook is skipped when the manifest already records the same
   (source content hash, cooker version) pair — content-keyed, mtime-independent. (M6.2,
   exercised hard by M6.6's viewer meshes)

9. **Parsing dependencies (license-gated per `third_party/README.md`):** the `gltf` crate
   (Apache-2.0/MIT) for glTF, `image` (MIT) for PNG/JPEG decode, `clap` (Apache-2.0/MIT)
   for the CLI, and tangent generation via the `mikktspace` Rust port (MIT/Apache-2.0) so
   our tangents match the de-facto interchange convention — with the derivation still
   written up in `docs/math/tangent-space.md` (M6.4), and an own implementation as the
   recorded fallback if the crate's API fights indexed meshes. STL is parsed by our own
   ~100-line importer (M6.6) — no dependency earns its keep there.

10. **Hot reload is a seam, not a feature.** The registry API is shaped so "re-cook, then
    swap a handle's target and bump its generation" can arrive later without an interface
    break; nothing in M6 implements it. (M6.1, documented in `docs/design/assets.md`)

## Consequences

**Good**

- The engine ships **no source-format parsers**: smaller attack/bug surface at runtime, and
  the trust-nothing reader is small enough to test exhaustively (truncation at every field,
  the M6.1 negative battery).
- Content-hash identity makes the cook cache, duplicate-load coalescing, and byte-stability
  checks all fall out of one mechanism — and renames stop being data-loss events.
- Schema hashes turn "cooked data drifted from the code" from silent corruption into a
  one-line error, and the same mechanism is pre-paid for the editor and replication.
- Deterministic cooks make cooked data diffable, cacheable, and — the real prize —
  a foundation M8's seeded fracture and M11's event replication can *prove* things against.
- Attribute flags let the mesh format grow through M6.4/M6.7 (and M10.4's SDF payloads,
  M8.1's destructibles) without a v2 container.

**Costs we accept**

- **A cook step exists at all**: you cannot point the engine at a `.gltf`. Samples ship
  with a cook fixture (CI cooks before ctest), and the friction is the price of the
  boundary. Mitigated by the cache (recooks are no-ops) and `rime-cli`'s ergonomics.
- **FNV-1a is not cryptographic** — an adversary can forge a collision. Asset files are
  not an adversarial input channel in v1 (they're your own build products); if signed
  content ever matters, the header's version field admits a stronger hash.
- **One file per asset** means many small files for texture-heavy content; packs are the
  planned, format-compatible answer when file-count pain is measured, not before.
- **Rust and C++ both know the container format** (writer and reader). The format spec
  lives in this ADR; the cross-language golden-fixture test (M6.2 regenerates what M6.1
  reads in CI) is the drift alarm.

## Alternatives considered

- **Load glTF directly at runtime.** Maximum convenience, and what many hobby engines do.
  Rejected: it drags a full scene-format parser (and its failure modes) into the runtime,
  couples engine data layout to an interchange format, and forfeits the offline work
  (mips, tangents, dedup, fracture, SDFs) that cooking exists to do. glTF is a great
  *import* format; it is not an engine's memory layout.
- **Use an existing serialization framework (FlatBuffers/protobuf/serde-bincode) for the
  container.** Buys schema tooling, costs a code-generation dependency in the C++ hot
  path, an opaque wire layout to teach around, and two languages' worth of generated glue.
  Our payloads are few, flat, and performance-shaped (vertex blobs, pixel data); a
  hand-written, documented, bounds-checked reader is smaller than the framework's
  integration and is itself teaching material — the same call the S0.4 protocol made.
- **UUID/GUID identity with a sidecar database (Unity-style `.meta`).** Stable across
  content edits (nice), but demands sidecar-file hygiene, merge-conflict policies, and a
  database the runtime must consult. Content-hash + path-hash + a regenerable manifest
  gives identity, caching, and dedup with **no state that can lie** — the manifest is
  derived, never authored. If human-stable ids become necessary (M9-era references from
  scenes into *renamed* sources), a name→id alias table can layer onto the manifest.
- **xxHash/BLAKE3 instead of FNV-1a.** Faster (xxHash) or cryptographic (BLAKE3) — both
  are dependencies or nontrivial vendored code for a property we don't need yet; FNV-1a is
  five lines we can teach. Revisit via the versioned header if hashing ever shows up in a
  cook profile or a trust boundary appears.
- **KTX2/DDS as the cooked texture container.** Standard and tooling-rich, but a second
  container grammar (and dependency) inside our container for v0's plain RGBA8+mips is
  weight without benefit; the payload can *become* KTX2-shaped when BCn adoption gives a
  reason.

---

*This ADR is brick **M6.0**. The code lands next: **M6.1** `engine/assets` + the RMA1
reader/registry; **M6.2** the Rust pipeline + glTF mesh cook + `rime-cli cook`; **M6.3**
textures (offline mips, sRGB/linear, per-mip RHI upload); **M6.4** materials + the PBR
texture upgrade (tangents, normal/MR/AO/emissive); **M6.5** async loading on the job
system; **M6.6** STL import + the viewer dogfood; **M6.7** skeletal-animation import;
**M6.8** the SDK (install/export); **M6.9** the C-ABI FFI crate; **M6.10** the proof sample
`samples/08-gltf-zoo` + docs. See [ROADMAP.md](../ROADMAP.md) → M6.*
