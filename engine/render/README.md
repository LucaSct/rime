# engine/render — the render graph & high-level renderer (M5)

`rime::render` is "where the picture comes from" ([ARCHITECTURE.md](../../docs/ARCHITECTURE.md)):
the layer that turns a *described frame* into recorded RHI commands. Its foundation is the
**render graph** ([ADR-0019](../../docs/adr/0019-render-graph.md)) — each frame, render code
declares passes and the resources they read/write; the graph compiles the execution order,
culls dead work, allocates transients from a cross-frame cache, emits every between-pass barrier
through the RHI's `texture_barrier` seam, and records the passes bracketed by GPU timestamps and
debug labels. This is the structure that makes UE5-class techniques tractable, and it is "the
home for M10": GI, virtual shadow maps, and many-lights arrive later as passes and resources on
this graph, not as a renderer rewrite.

## Status (built bottom-up — see [docs/ROADMAP.md](../../docs/ROADMAP.md) → M5)

| Brick | Provides | State |
| --- | --- | --- |
| M5.4 | **RenderGraph v0** — frame-declared raster/compute passes, virtual resources (`RGTexture`), `import`/`export`, compile (versioning → edges → topo order → cull), the transient cache, graph-owned barriers, per-pass GPU ms | landed |
| M5.5 | the **scene layer** — `OrbitCamera` graduated from the viewer (ADR-0016 rule 3, the first parallel-path promotion), procedural primitives (plane/cube/uv-sphere, analytically exact) + `MeshRegistry`/`MaterialRegistry` behind dense ids, reflection-registered ECS render components (`MeshRef`, `MaterialRef`, `Camera`, lights) | landed |
| M5.6 | the PBR forward pass library (depth pre-pass → HDR forward → tonemap) + `docs/math/pbr.md` | landed |
| m10.1 | **directional cascaded shadow maps** — cascade fit, layered depth array, hardware-PCF compare + `docs/math/shadow-mapping.md` | landed |
| m10.2 | **local (spot) shadows with a destructibility-aware cache** — a slot re-renders only when its light moves or a destruction event's AABB touches its frustum | landed |
| m10.3 | **clustered forward shading** — froxel light culling in compute + `RGBuffer` + `docs/math/clustered-shading.md` | landed |
| m10.4b | **the runtime SDF clipmap** — 3 nested 64³ R16Snorm levels composed from m10.4a's cooked per-mesh/part fields by a min-blending compute pass, camera-centred with per-level texel-snapping, dirty-tracked via C1 (instance move/add/remove) and C2 (destruction events) so a static scene settles to zero work and one broken wall recomposes only its own neighbourhood + `docs/math/sdf.md` §6–10. Not yet sampled by anything (m10.5's DDGI probes are the first consumer); gated off by default. | landed |
| m10.4c | **the GI feasibility spike** — ADR-0032 §2's named unknown, measured: sphere-trace cost at DDGI probe-grid scale, and what it fixes about m10.5's probe count / rays-per-probe / update policy. Findings and the recommended starting configuration in [`docs/design/gi-spike.md`](../../docs/design/gi-spike.md); the sweep itself is a shape-only test case in `tests/render/sdf_clipmap_test.cpp`, so it re-measures on real hardware at m12.0. | landed |

Deliberate v0 bounds (ADR-0019 records why): serial recording on one queue (pass boundaries keep
the parallel-recording and async-compute seams open) and no transient aliasing (measure first).
Buffers joined the graph as first-class resources at m10.3, with clustered forward's light lists as
the first GPU-driven consumer ADR-0019 predicted.

## Layout

```
include/rime/render/   # public interface
  render_graph.hpp     #   RenderGraph, RGTexture, RGBuffer, pass descriptors
  lighting/            #   the M10 lighting techniques (shadows, clustered, …)
src/                   # the compiler + executor
```

Depends on `rime::rhi` only (the scene layer adds `rime::ecs` at M5.5). Tests live in
`tests/render/` — pixel-verified, GPU-free on lavapipe like every proof since M3.3.
