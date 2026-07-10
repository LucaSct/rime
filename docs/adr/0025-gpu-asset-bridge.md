# ADR-0025: The GPU asset bridge — where cooked assets become GPU resources

- Status: Accepted
- Date: 2026-07-10

## Context

M6.5 gave the engine **asynchronous asset loading**: `AssetServer::request_*` returns a handle
immediately and fans the file read + decode + validate out as a job, so a "Ready" asset is
*CPU-resident and validated* — not yet on the GPU. M6.4's renderer, meanwhile, draws from
hand-built `PbrMaterialDesc`s whose texture handles are, in the header's own words, "BORROWED...
until the M6 loader gives textures a real home." Nothing yet uploads a cooked `.rtex`/`.rmesh`/`.rmat`
to the GPU. Closing that gap needs one decision made deliberately, because it is an inter-module
edge: **where does the code that turns a CPU-resident cooked asset into a live GPU resource live?**

The forces:

- **`engine/assets` must stay device-agnostic.** It links only `core` + `platform` (verified), which
  is what keeps the whole parse/validate path unit-testable with in-memory buffers and no Vulkan
  device. Uploading is a GPU concern; putting `create_texture`/`write_texture_mips` behind the asset
  layer would drag the RHI into it and break that property.
- **The render layer already owns the GPU-side registries.** `rhi::Device`, `MeshRegistry`, and
  `MaterialRegistry` all live in `engine/render`; a `PbrMaterialDesc` already carries the five
  `rhi::TextureHandle`s an uploaded texture must fill. The upload target is *here*.
- **The dependency direction is settled.** ADR-0016's modularity rule and the M6.1/M6.5 notes state
  it plainly — "the renderer *consumes* assets and owns GPU residency; the dependency never points the
  other way." `AssetServer::pump()` even documents itself as "the frame point the render layer's
  GPU-upload drain will hang off."
- **The upload primitive already exists.** M6.3 built `Device::write_texture_mips` (per-level upload
  of an offline, gamma-correct mip chain) but shipped it with **no consumer** — it was built *for* this.

## Decision

The **GPU asset bridge lives in `engine/render`** (`gpu_asset_bridge.{hpp,cpp}`), and `rime::render`
gains a `PUBLIC` dependency on `rime::assets`. This is the single allowed `assets → render` edge, and
it points the sanctioned way: render consumes assets; assets never links render or the RHI.

The bridge is a thin, frame-thread object:

- It holds refs to `rhi::Device` + `AssetServer` and an **upload-once cache** (`TextureAssetHandle →
  rhi::TextureHandle`). It uploads the AssetServer's magenta placeholder once, so a still-loading
  texture always resolves to a valid handle (no per-bind "is it loaded?" branch).
- `drain()` runs each frame **right after `AssetServer::pump()`**: for every requested texture now
  `Ready` and not yet resident, it `create_texture` + `write_texture_mips` (verbatim, per the offline
  chain) and caches the handle. `texture_or_placeholder()` returns the real texture once resident,
  else magenta — the swap a material sees.

*Rejected:* a standalone `engine/asset_gpu` module. It would fragment render's ownership of its own
GPU resources, and would have to depend on render anyway to reach `PbrMaterialDesc`/`MeshRegistry` —
a new module boundary bought nothing.

## Consequences

- Anything linking `rime::render` now transitively links `rime::assets`. Acceptable: render is the
  top of the engine's GPU stack; nothing below the samples links it.
- The `drain()`-after-`pump()` seam and the placeholder swap generalize unchanged to **meshes**
  (`MeshAsset → MeshRegistry`) and the **material pipeline** (resolving a `MaterialAsset`'s texture
  AssetIds into a `PbrMaterialDesc`) — the next bricks on this ladder.
- This is the first consumer of `write_texture_mips`; the M6.3 upload path is now exercised end to end
  by a GPU proof (magenta placeholder → real cooked checker on `drain()`, `tests/render/pbr_pipeline_test.cpp`).
- Roadmap: this is the work M6.5's notes mis-labelled "M6.6" (which became the STL dogfood). It lands
  as its own brick before M6.10's `08-gltf-zoo`, which needs cooked textures on screen.
