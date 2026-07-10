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
6       2     asset_kind         u16, LE           (Mesh = 1; Texture = 2; Material = 3; …reserved)
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

### The texture payload (`asset_kind = Texture`) — M6.3

```
u32   width                  base level extent
u32   height
u32   format                 0 = RGBA8 linear (UNORM), 1 = RGBA8 sRGB   (BCn values reserved)
u32   mip_count              a full chain: floor(log2(max(width,height))) + 1 levels
      mip_count × { u32 width, u32 height, u32 offset, u32 size }   the mip table
byte  pixels[Σ size]         every level's RGBA8 texels, level 0 first, tiled at the table's offsets
```

A cooked texture carries a **full, offline-generated mip chain**. The chain is box-filtered in
**linear light**: for an sRGB texture the cooker linearises each texel, averages, then re-encodes, so
minified colour surfaces don't darken (the classic gamma-wrong-mips bug — a black/white checker's
coarse mip is sRGB ~188, *not* the too-dark 128 a naive byte average gives). Linear textures
(normal / metallic-roughness / occlusion — data, not colour) are averaged directly; alpha is always
linear. The colour space is the cooker's call — a material's usage picks it from M6.4, and a
standalone `rime cook` takes `--srgb`/`--linear`. The engine uploads each level **verbatim** through
the RHI's `write_texture_mips` (per-mip buffer→image copies, no GPU blit) — it never regenerates the
chain, because the cooked mips are the gamma-correct ones. The reader cross-checks every level's
extent against the base halved to that level, its `size` against `width·height·4`, and the offsets
against a gap-free tiling of the blob, so a corrupt table can never make an upload read past the
pixels. The in-memory `TextureAsset`'s `mips[i]` slice `pixels` directly.

### The material payload (`asset_kind = Material`) — M6.4

```
f32   base_color[4]          linear RGBA factor            (glTF default 1,1,1,1)
f32   emissive[3]            linear RGB factor             (glTF default 0,0,0)
f32   metallic               0 = dielectric … 1 = metal    (glTF default 1)
f32   roughness              0 = mirror … 1 = rough        (glTF default 1)
f32   normal_scale           scales the normal-map XY      (glTF default 1)
f32   occlusion_strength     lerps AO toward "none"        (glTF default 1)
f32   alpha_cutoff           the Mask threshold            (glTF default 0.5)
u32   alpha_mode             0 = Opaque, 1 = Mask, 2 = Blend
u64   base_color_tex         AssetId of a cooked texture   (0 = none → engine fallback)
u64   metallic_roughness_tex AssetId                       (0 = none)
u64   normal_tex             AssetId                       (0 = none)
u64   occlusion_tex          AssetId                       (0 = none)
u64   emissive_tex           AssetId                       (0 = none)
```

A material is a **fixed 92-byte record** — no variable-length tail — so the reader reads it straight
through and requires exactly that length (no short read, no trailing bytes). It is the metallic-
roughness parameter set (the shared vocabulary of glTF/UE/Unity/Frostbite) plus **references** to the
textures that drive it. A texture slot is not pixels but an `AssetId` — the content hash of an already-
cooked texture — so one GPU upload is shared across every material that names the same image, and a
slot of `0` means "no texture" (the renderer binds a 1×1 white / flat-normal / white-AO fallback so a
single shader serves every material permutation). Colours are linear (the cook converts sRGB authoring
values); each texture's colour space follows its usage (base-color/emissive sRGB, the rest linear).
The reader additionally rejects an unknown `alpha_mode` and any non-finite factor, so a NaN never
reaches the shader. Materials are emitted from a glTF alongside its meshes, never cooked standalone.

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

`tests/assets/cooked_mesh_test.cpp` (and `cooked_texture_test.cpp` for the texture path: unknown
format, a mip table inconsistent with the base extent, a short or over-long pixel blob) drives one
crafted file per failure mode — including truncation at *every* byte length — and the suite is
ASan/UBSan-clean. This is the same posture as the S0.4 protocol decoder; the negative battery is the
proof.

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

Textures use the same mechanism, fingerprinting the v1 `{width, height, offset, size}` **mip-record**
(the repeated unit the reader walks) — pinned at `0xAB8A2B884141F736`, cross-checked by the M6.3
`checker.rtex` golden fixture. Only the record is hashed, not the base-extent/format header around it:
a future *pixel format* is an appended `TextureFormat` value (old files stay valid), not a layout
change, so it needs no re-cook.

Materials use it too, and here the fingerprinted record is the **whole** payload (a material has no
variable-length tail): the factor and texture-reference fields, pinned at `0xCA4ED4CC434C941A`. Both
languages embed that constant, so the cooked-material format agrees across the C++ reader and the Rust
`material.rs` cooker by construction. Adding a material property later (a new factor, a KHR-extension
knob) changes the fingerprint — a deliberate re-cook — which is why the container leaves room for the
kinds that will reference materials without touching this record.

## Determinism

Cooked output must be a pure function of its input bytes and the cooker version (ADR-0024, decision 7):
no timestamps in payloads, no hash-map iteration order leaking into output (sorted traversal), version
stamped in the manifest. That byte-stability is what M8's seeded fracture and M11's event replication
stand on. Cross-*machine* cook identity is not promised (float codegen may differ); cooked data is
built on one machine and shipped as data.

## Loading at runtime — the async `AssetServer` (M6.5)

The M6.1 registry loads *synchronously*: read the file, validate it, keep it — all on the calling
thread. That stalls the frame the moment content scales. The `AssetServer` (`engine/assets/
asset_server.hpp`) makes loading **structurally asynchronous** without pushing that complexity onto
its callers:

- **`request_mesh(path)` / `request_texture(path)` return immediately** with a small
  `AssetHandle<T>` (a dense index, phantom-typed on the asset kind so a mesh handle can't be passed
  where a texture handle is wanted). The load — IO via `platform::read_file`, then the M6.1 reader's
  parse+validate, which is pure — fans out as a job on the `core::JobSystem`.
- **States move `Loading → Ready | Failed`, one way.** *Ready* means CPU-resident and fully
  validated — **not** uploaded to the GPU. `engine/assets` depends only on `core` + `platform`
  (never the RHI), so device residency is the render layer's job, drained on the frame thread at
  M6.6. Keeping the seam here, not at the upload, is what keeps the asset layer device-agnostic.
- **A not-yet-ready handle resolves to a visible placeholder** — a unit cube for meshes, a 2×2
  magenta/black checker for textures (magenta being the universal "missing texture" tell).
  `get_or_placeholder(handle)` *never* returns null, so the render extraction records a draw for a
  pending asset without a branch; `get(handle)` returns `nullptr` until Ready for callers that care.

Two contracts keep this both safe and simple (measure before cleverness — one mutex, not a lock-free
maze):

- **Threading.** `request_*` may be called from the main thread *or from within a running job* (the
  JobSystem's own submit rule; arbitrary foreign threads are not allowed, per the Chase-Lev deque's
  single-owner rule). All bookkeeping lives behind one mutex; the heavy work — file read + decode —
  runs *outside* the lock, so loads are genuinely parallel. `pump()` and the getters are main-thread.
  Crucially, a completed load queues its result and leaves the slot in `Loading`; **only `pump()`
  (main thread) flips a slot to `Ready`**, so a getter and a finishing job never race over a slot's
  payload. `wait_for_pending_loads()` drains in-flight loads (participating in the job system, so it
  never deadlocks); the destructor calls it first, so no job outlives the queues it writes into.
- **De-duplication.** Repeat requests for the same *path* coalesce onto the first handle — one
  physical file read per path, whatever the request storm. `physical_load_count()` is that counter,
  and the proof asserts *N requests over K paths ⇒ K loads*. This request-path coalescing is distinct
  from, and composes with, the registry's *content-hash* de-dup on the decoded bytes.

Proof: `tests/assets/asset_server_test.cpp` — 64 requests fanned across worker jobs coalesce to
exactly 8 loads with every handle reaching Ready and the async bytes equal to a synchronous read
(no mis-slotted load); placeholder-until-`pump()`; and missing/corrupt files fail cleanly while the
placeholder persists. This is the milestone's threading brick, so it runs under the CI
ThreadSanitizer job.

**Out of scope (documented seams):** load priorities, memory budgets, eviction, hot reload,
decompression jobs (payloads are uncompressed per ADR-0024). The `pump()`/upload split is where a
per-frame upload *budget* will live at M6.6 — a value passed to the drain, not a later refactor.

## Seams left open (not built in M6.1)

- **Hot reload.** The registry hands out generational handles, never raw pointers into relocatable
  storage. That indirection is exactly what "re-cook, swap the slot's target, bump its generation"
  needs, so hot reload can arrive later without an interface break. Nothing implements it yet.
- **Async loading.** *Built in M6.5* — see *Loading at runtime* above. Loads run on the `JobSystem`
  behind placeholder assets; the GPU-upload half lands with the render bridge at M6.6.
- **Packs / bundles.** One asset per file today; the standalone header admits a pack layer later.
- **Compressed textures (BCn), KTX2/DDS.** The format enum reserves values; adopted only when a
  measured GPU-memory need appears (VISION: measure before optimize).
- **A shared byte cursor for `engine/stream`.** The stream protocol predates `core/byte_cursor.hpp`
  and keeps its own private copy; it can adopt the core one later (a mechanical change).
