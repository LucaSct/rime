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
| M5.5 | the scene layer: `OrbitCamera` (graduated from the viewer), mesh/material registries, ECS render components | next |
| M5.6 | the PBR forward pass library (depth pre-pass → HDR forward → tonemap) + `docs/math/pbr.md` | planned |

Deliberate v0 bounds (ADR-0019 records why): serial recording on one queue (pass boundaries keep
the parallel-recording and async-compute seams open), no transient aliasing (measure first),
graph resources are textures (buffers join with their first GPU-driven consumer).

## Layout

```
include/rime/render/   # public interface
  render_graph.hpp     #   RenderGraph, RGTexture, pass descriptors
src/                   # the compiler + executor
```

Depends on `rime::rhi` only (the scene layer adds `rime::ecs` at M5.5). Tests live in
`tests/render/` — pixel-verified, GPU-free on lavapipe like every proof since M3.3.
