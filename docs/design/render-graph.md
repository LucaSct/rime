# Design note: the render graph (skeleton — M5.0)

Companion to `engine/render` and [ADR-0019](../adr/0019-render-graph.md), which settles the
architecture. Written as a skeleton at M5.0; filled in at **M5.4** with the shipped design and
the measured numbers (below).

## Why a graph at all

A frame is many passes reading and writing many GPU resources. Hand-rolled, every pass author
must know every other pass's formats, layouts, and lifetimes — O(N²) knowledge that stops scaling
around a handful of passes (the ICEM viewer's three-pass frame is our own evidence). Declaring
the frame as **data** — passes + the resources they touch — gives one place that owns ordering,
transient memory, synchronization, and timing. That structure is what makes UE5-class rendering
tractable, and it is "the home for M10" (ARCHITECTURE → `render`).

## The mental model (decided — ADR-0019)

- **Declared fresh each frame.** Setup lambdas declare passes; compile orders them and derives
  everything else; execute records; the graph is discarded. Nothing is retained but the physical
  transient cache.
- **Virtual resources.** `create_texture(desc)`/`create_buffer(desc)` return ids; memory exists
  only after compile, drawn from a desc-keyed cross-frame cache. External resources (backbuffer,
  readback/stream targets, later persistent history) enter via `import(handle, state)`.
- **Declared access is the single source of truth.** `ColorWrite`, `DepthRead`, `Sampled`,
  `StorageWrite`, … per (pass, resource). From it the graph derives: dependency edges (via
  resource versioning), the topological pass order (declared order breaks ties), pass culling,
  and **every barrier** — emitted as explicit RHI transitions (the seam
  `command_buffer.hpp` reserved since M3). Graph-less code keeps the old implicit tracking.
- **Raster / compute / copy pass kinds.** Raster passes declare attachments; the graph owns the
  `begin_rendering`/`end_rendering` scope; the lambda only binds and draws.
- **Serial single-queue v0, seams kept.** Pass boundaries + declared access are exactly what
  parallel per-pass recording (JobSystem) and async compute (M10) need later; both are
  measurement-gated upgrades that change no declarations.
- **Timing built in.** Per-pass GPU timestamps + debug labels from the first frame.

## API sketch (v0 target — final signatures land with M5.4)

```cpp
render::RenderGraph graph(device);

RGTexture hdr   = graph.create_texture({.extent = view, .format = Format::RGBA16Float,
                                        .usage = ColorAttachment | Sampled, .debug_name = "hdr"});
RGTexture depth = graph.create_texture({.extent = view, .format = Format::D32Float,
                                        .usage = DepthStencil, .debug_name = "depth"});
RGTexture back  = graph.import(swapchain_image, ResourceState::Undefined);

graph.add_raster_pass("depth-prepass", {.depth = {depth, LoadOp::Clear}},
    [&](rhi::CommandBuffer& cmd) { /* bind depth-only pipeline, draw scene */ });

graph.add_raster_pass("forward-pbr",
    {.colors = {{hdr, LoadOp::Clear}}, .depth = {depth, LoadOp::Load, /*read-only*/ true}},
    [&](rhi::CommandBuffer& cmd) { /* draw lit scene */ });

graph.add_raster_pass("tonemap", {.colors = {{back, LoadOp::DontCare}}, .reads = {hdr}},
    [&](rhi::CommandBuffer& cmd) { /* fullscreen triangle sampling hdr */ });

graph.execute(cmd_buffer);   // compile → transients → barriers → record, in declared-data order
```

## Compile, step by step (M5.4 — `render_graph.cpp`)

1. **Edges by resource versioning.** Walk passes in declared order tracking, per resource, the
   *last writer* and the *readers of that version*. A read depends on the last write (RAW); a
   write depends on the last write (WAW) **and** every read of the old version (WAR — it must not
   clobber data a reader still needs). This is SSA thinking applied to GPU resources: each write
   mints a new version, and edges connect versions to their consumers. One consequence worth
   internalizing (it is pinned by a test): a pass declared *before* a resource's writer reads the
   **older version** — declaration order *is* data-flow order, never something the graph
   second-guesses.
2. **Liveness flows backwards.** Seed with passes that write anything *observable* (an imported
   or exported resource), then walk the reverse edges: whoever a live pass depends on is live.
   Everything else is **culled** — declared, ordered, never executed.
3. **Topological order, declared order breaking ties** (Kahn's algorithm, lowest declared index
   first). With versioned edges the declared order is always *a* valid topological order, so the
   sort never surprises — its value is validation (a cycle is a caller bug caught by assert),
   determinism, and the grouping information parallel recording will use later.

## Barriers: from declared access to synchronization2 (M5.4)

Each declared access names a `rhi::ResourceState` — the RHI's coarse spelling of layout +
stage/access (`ColorTarget`, `DepthTarget`, `ShaderRead`, `StorageReadWrite`, `TransferSrc/Dst`,
`Present`). While recording, the graph tracks each resource's current state and emits
`cmd.texture_barrier(physical, from, to)` whenever a pass needs it in a different one; the Vulkan
backend maps both states through one table (`to_vk(ResourceState)`) into a precise
synchronization2 image barrier and keeps its tracked layout in agreement.

**The deliberate split:** attachment states are delegated to `begin_rendering`'s existing tracked
transitions (which double as the write-after-write barrier between passes hitting the same
target); the graph emits the transitions the backend *cannot* see coming — sampled/storage reads
of a texture some earlier pass wrote, the exact hazard hand-rolled multi-pass code gets wrong
first. One owner per kind of knowledge: frame-global read hazards in the graph, attachment
mechanics in the backend. Full graph ownership of every transition arrives with parallel
recording, when per-encoder tracked state stops existing.

## The transient cache (M5.4)

`create_texture` records a desc; memory exists only after compile. Physicals are satisfied from
a cache keyed by **(extent, format, accumulated usage)** — usage is accumulated from the declared
accesses themselves (an `RGTextureDesc` deliberately has no usage field, so declaration and usage
cannot disagree; exported textures get `TransferSrc` added because "export" means someone outside
reads them). A transient costs a real allocation the first frame its key appears, then recycles:
`reset()` marks every cached texture free, `execute()` claims them back. Two accepted v0 bounds,
both measured before changed: no **aliasing** (two transients with disjoint lifetimes could share
memory; ADR-0019 defers this until a real workload shows the win) and no **eviction** (the cache
grows to the high-water mark of distinct keys).

## Buffers are resources too (`RGBuffer`, m10.3)

Textures were the graph's only resource kind until clustered forward shading needed a compute pass
to *fill a light list* that a later raster pass *reads*. `RGBuffer` / `create_buffer` /
`import_buffer` make that pair visible to the compiler, and the payoff is not one feature but
three, all of which fall out of the existing machinery for free:

- **Ordering.** The cull dispatch writes the lists; the forward pass declares them read; the
  versioning walk turns that into an edge.
- **Liveness.** A dispatch that wrote nothing declared would be *culled as dead* — which is exactly
  what happened before the buffer was declarable. Declaring the write is what keeps it alive (and
  `export_buffer` is how a test that reads the lists back keeps its producer alive with no
  consumer pass at all).
- **Barriers.** A buffer read is declared `ShaderRead` and a write `StorageReadWrite`, so the
  state-change rule that already emitted texture transitions emits `buffer_barrier` between them.

Textures and buffers share one resource table, one index space, and every compiler step; only
allocation (a size-keyed buffer cache alongside the texture one) and the final transition differ.
`RGTexture` and `RGBuffer` stay distinct C++ types, so mixing them up is a compile error.

## Measured overhead (M5.4, this repo's dev server)

A synthetic worst case — a 100-pass chain, each pass sampling its predecessor's output (maximal
edges) — **declares + compiles + records in ~1.3 ms in a Debug build** on the 16-core dev server
(lavapipe; `tests/render/render_graph_test.cpp` prints it). The real M5 frame is 3–6 passes, i.e.
noise. Per ADR-0019: if a profile ever disagrees at real pass counts, caching a compiled schedule
keyed by the declaration is a contained optimization — but at these numbers, rebuilding the frame
as data every frame costs nothing worth designing around.
