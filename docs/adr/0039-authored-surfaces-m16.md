# ADR-0039: M16 — "Authored Surfaces", and the four decisions the material path needs

- Status: Proposed
- Date: 2026-09-02

## Context

Rime can cook textures and it can render them, but not through its own asset path.

m15.1 made a scene name a mesh by content id and draw it. It stopped one field short: the entity
then gets a **hardcoded neutral grey** (`engine/render/src/gpu_asset_bridge.cpp:119-125`), assigned
only when the entity has no `MaterialRef`, and — because the idempotence check at `:194`
short-circuits — **never revisited**, even after the real mesh streams in. Cooked materials are
decoded by nobody: `assets::MaterialAsset` and `read_material` exist, and outside tests their only
C++ caller is `samples/08-gltf-zoo`, which builds its world by hand rather than from a `.rscene`.

So every asset class a game needs — weapons, buildings, towns, vegetation, terrain — dead-ends at
grey unless a sample hand-carries it. A review pass over the tree found three further facts that
make this larger than "wire up materials", each verified against the code rather than assumed:

- **No game resolves scene assets at all.** `resolve_scene_meshes` has exactly two callers: the
  editor host and one test. `GpuAssetBridge` appears in no sample except `08-gltf-zoo`. The game
  half of "load a scene and see it" is not a missing per-frame pump; it is a missing runtime.
- **Alpha masking is broken in the primary view, not only in shadows.** The depth pre-pass has no
  fragment shader (`engine/render/src/passes.cpp:81-97`), so it writes depth for masked-out texels.
  The forward pass then discards there, and the geometry behind fails `CompareOp::Equal`
  (`passes.cpp:179`) and is never shaded — the hole renders as clear colour, a cutout in front of a
  lit wall. The pre-pass is **on by default** and every production call site enables it. m15.6a's
  own proof cannot see this: `tests/render/pbr_pipeline_test.cpp:591` passes
  `use_depth_prepass=false`, the one configuration no caller uses.
- **`alpha_cutoff` is always 0 through the engine's asset path**, because nothing in `engine/`
  reads `assets::AlphaMode` — the single read in the tree is in the zoo sample
  (`samples/08-gltf-zoo/main.cpp:405`). m15.6a made the shader capable and wired one sample; it did
  not wire the asset pipeline, because that pipeline does not exist yet.

Masking, per-submesh materials and authored colour are therefore all blocked behind one missing
link, and fixing that link is what this milestone is.

### The fact that decides the architecture

`AssetId = fnv1a_64(payload)` (`tools/asset-pipeline/src/cooked.rs:155-165`), and the **schema hash
lives in the header and is not itself hashed**. Two consequences follow, and they point in opposite
directions:

- A **schema bump churns no ids.** Adding a field to the cooked material record changes
  `material_schema_hash()` but not any `AssetId`.
- A **payload content change churns ids, transitively.** Texture bytes change (block compression,
  mip-alpha coverage) → texture ids change → material payloads *embed* texture ids → material ids
  change.

Nothing in that chain churns **mesh** ids — unless material ids are put into the mesh payload,
which is the tempting fix for the missing mesh→material edge. And mesh ids are precisely what
`.rscene` files carry. Putting material ids in the mesh payload would mean that recompressing a
texture silently invalidates every scene file in the project.

## Decision

**M16 is "Authored Surfaces."**

> **Done when:** a texture authored in Blender is cooked, placed in a `.rscene`, and renders on that
> mesh in **both the game and the editor viewport** — provable by a proof brick whose own diff
> touches only `assets/`, `samples/` and `docs/`.

That is the M15 discipline applied again, and it is the only formulation here that can fail
honestly: anything the proof needs an engine edit for is a missing brick, not part of the proof.

**The existing M16 "The Visual Bar" becomes M17.** This milestone is a prerequisite for it — a
visual bar cannot be judged on content that renders grey.

### Ruling 1 — the mesh→material edge is the manifest's `#materialN` convention, resolved engine-side

The cook already emits one manifest line per material with `source_path = "<input>#material<N>"`
(`tools/asset-pipeline/src/lib.rs:230-240`), and the cooked mesh already carries
`Submesh::material_slot` (`engine/assets/include/rime/assets/mesh_asset.hpp:107`). The bridge joins
them: `find_by_id(mesh)->source_path + "#material" + slot` → `find_by_source` → material id.

**Rejected: a material-id table in the cooked mesh payload.** It is the more obviously "typed"
design, and it is the one that makes every texture recompression invalidate every `.rscene`, for the
reason in the Context. Rejected on that ground alone.

**The condition that reverses this ruling:** a manifest that grows a typed reference column, at
which point the edge stops being a string convention and ids stop being the join key. Until then
the convention is load-bearing runtime API and is documented as such rather than left implicit.

### Ruling 2 — one `DrawItem` per submesh, and the entity holds a material *set*

A multi-material glTF already cooks to one `.rmesh` with one submesh per primitive
(`tools/asset-pipeline/src/mesh.rs:107-118`), and the reader already validates the table
(`engine/assets/src/cooked_reader.cpp:400-413`). The data then dies at
`engine/render/src/mesh.cpp:296`, because `CpuMesh` has no field it could live in. The runtime
consequence is one material per drawn mesh — which is not how Blender wants to work, and not how any
other engine behaves.

Components are trivially-copyable PODs, so an entity cannot hold a `std::vector<MaterialId>`. The
entity therefore carries a `MaterialSet{MaterialSetId}` indexing a small registry, and extraction
emits one `DrawItem` per submesh, repeating the entity in the parallel `draw_entities` array.

**Rejected: one child entity per submesh** — it pollutes the outliner and the saved scene file with
entities the author never made. **Rejected: splitting each submesh into its own registry mesh at
upload** — free, because the cook already concatenates disjoint per-primitive vertex blocks, but it
turns one placed object into N scene objects and breaks picking's answer to "what did I click".

### Ruling 3 — `DrawItem`'s shape is decided once, here

```
DrawItem { mesh, material, model, first_index, index_count, flags }
```

`flags` bit 0 = alpha-masked, bit 1 = double-sided. Three separate bricks in this milestone consume
that word; deciding it once is what stops each of them rewriting `record_draws` and the seven
parallel arrays that travel with the draw list.

### Ruling 4 — derived components are never saved

`MeshRef`, `MaterialRef` and `MaterialSet` are **derived**: they are dense indices into runtime
registries, minted per session by the asset bridge. `save_scene_to_string` currently serializes every
reflected component, so saving a scene the viewport has resolved writes session-local indices into
the authored file — Ctrl+S with zero edits corrupts it, and per-submesh material sets make the
corruption bigger. The writer gains an exclusion list, and **a counter proving the exclusion ran**,
because an exclusion that silently stops working is indistinguishable from one that never had
anything to exclude.

### Two seam calls

**Dynamic cull state.** Double-sided materials need per-draw cull. The forward pass already bakes
six pipeline variants (`passes.cpp:168` propagates `CullMode::Back` to all of them); a cull axis
would make twelve, and the depth pass four. We take `VK_DYNAMIC_STATE_CULL_MODE` — Vulkan 1.3 core,
supported by lavapipe — and add one RHI surface, `CommandBuffer::set_cull_mode`. Adding a method to
the RHI is an ADR-level act under guardrail 1, which is why it is named here rather than decided
inside a brick.

**Block-compression fallback.** A device without `textureCompressionBC` must **fail the load with a
named counter and a warn-once**, never silently substitute the magenta placeholder. A fallback path
that nothing exercises is a fallback that does not work, and this engine has shipped that shape
before.

#### The BC spike, run before any of it was written (2026-09-02)

*Measured.* `textureCompressionBC = true` on both local devices — the RTX 3060 (NVIDIA) and the
integrated RADV. **Lavapipe, which is what CI actually runs (`mesa-vulkan-drivers`), is not installed
on this machine and was NOT verified.** That is deliberately not blocking: m16.1 adds the device
capability query the RHI has never had, and a test that *reports* BC support turns the question into
a fact CI answers permanently, rather than an assumption either way. If lavapipe says no, the honest
shape is a CPU encode→decode→error-bound proof plus an explicitly named unproven GPU gap — never a
GPU test that silently skips.

*Encoders.* `third_party/README.md`'s policy decides this more cleanly than a licence audit would:
"we don't pull in a library for something we should understand and own", and the directory "vendors
no dependency *source*" because "the source is still fetched and checksummed at build time". A BC7
crate that ships **prebuilt per-platform binaries** fails both clauses, on a three-OS CI, for a
format we can encode ourselves. So:

- **BC5 is written in-tree.** Two independent BC4 channels — min/max endpoints and 3-bit indices. It
  is genuinely small, it needs no dependency, and it doubles as the encoder harness.
- **BC7 is written in-tree, mode 6 only** (single subset, RGBA endpoints, 4-bit indices): ~250 lines,
  with mediocre quality on high-contrast blocks, documented as the known limitation and the reason a
  better mode search is a later brick. Taking a vendored-binary dependency to avoid writing 250 lines
  is the trade this repo's policy exists to refuse.

*Not verified:* crate licences were not audited directly — this machine has no network (crates.io
returns 403) — but the policy above makes the question moot rather than open.

### What M16 does not fix, said plainly

glTF `Blend` still draws as **Opaque**. There is no transparency pass and this milestone does not add
one. Without saying so here, "Blender textures reach the mesh" would be reported done while every
transparent material silently lies.

There is also **no authored `MaterialAsset{u64}` component.** The material is derived from the mesh,
engine-side. A draggable 64-bit content hash in an inspector is not authoring, and the asset browser
could not populate it correctly anyway: it holds only `{source_path, kind, id}` per manifest line and
never reads the cooked bytes where `material_slot` lives, so it would have to guess `#material0`.
This is the same gap m15.3 named when it ruled out an authored `Name` component — **reflection has
no asset-reference field type** — and it is recorded here as the same debt rather than worked around.

## Consequences

- **The `#materialN` string is now a runtime contract.** The cook may not change that naming without
  breaking scene loading. It is documented in `docs/design/assets.md` as a consequence of this ADR.
- **Every id-churning cook change must land before anything commits an id.** Block compression and
  mip-alpha coverage rewrite texture payloads, so they precede the authored proof asset in the brick
  ladder. This is why the milestone's headline lands before its riskiest brick but its *proof* lands
  after.
- **Two committed-fixture tests become weaker and need a partner.**
  `tools/asset-pipeline/tests/cook_fixture.rs` asserts the cooker still produces the committed
  bytes, so regenerating a fixture makes it green *by construction*. Every payload-changing brick
  therefore owes an additional assertion that the C++ reader **rejects** the old bytes with a
  specific schema mismatch. That is the only version of the test that can fail.
- **Material gains the cross-language fixture it never had.** It is currently the only cooked kind
  with no committed fixture and no cross-language load test, so a field-order transposition in the
  cooker would ship green in both languages. The double-sided brick adds one while it is bumping the
  schema anyway.
- **Picking must follow the draw path.** If the picker keeps rasterising whole meshes while the
  forward pass draws submeshes, a click is answered by a different rasterisation than the one on
  screen — a wrong answer with no wrong pixel anywhere, which is the hardest kind to notice.

## Alternatives considered

**Ratify one-material-per-mesh and warn at cook time.** Cheaper, and defensible: the author splits
by material at the Blender-object level. Rejected because it pushes an engine limitation into every
authoring session for every asset, and because the data to do it properly is already cooked,
validated, and merely dropped — the cost is a table on `CpuMesh`, not a format change.

**Add the authored `MaterialAsset` component anyway.** It would make the material overridable per
entity, which is eventually wanted. Rejected for now: it cannot be populated correctly by the one UI
that would produce it, and a wrong-by-construction authoring affordance is worse than an absent one.
Worth noting for whoever revisits: `MaterialAsset{uint64 asset}` would be *structurally identical* to
`MeshAsset{uint64 asset}`, and is safe only because [ADR-0033](0033-networking-v1.md)'s amendment A2
folds the type name into the type hash — `engine/core/include/rime/core/reflect/type_info.hpp:131-135`
names that exact pair as one of the collisions A2 fixed.

**Fold this work into M16 "The Visual Bar".** Rejected on the same grounds
[ADR-0038](0038-platform-proof-m15.md) used to split M15 from M16: the two have different proof
regimes. This milestone's proof is a cooked asset rendering in two hosts and is CI-gateable; the
visual bar's can only be judged on hardware. Bundling them yields one milestone whose "done when" is
two sentences joined by "and" — and M16 already carries M13's unmet frame-rate clause, which this
work would inherit for no reason.
