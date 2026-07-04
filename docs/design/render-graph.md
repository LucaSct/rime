# Design note: the render graph (skeleton — M5.0)

Companion to `engine/render` (not yet built) and [ADR-0019](../adr/0019-render-graph.md), which
settles the architecture. This note is a **skeleton** at M5.0: it fixes the mental model and the
API shape the bricks build toward, and is filled in with real code excerpts and measured numbers
at **M5.4**, when the graph lands. Sections marked ⏳ are written then. (Stub honesty per
CLAUDE.md.)

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

## ⏳ Compile, step by step (M5.4)
Resource versioning, edge derivation, the topological order and its tiebreak, culling — with the
real code and a worked example.

## ⏳ Barriers: from declared access to synchronization2 (M5.4)
The abstract `ResourceState` set, the transition table, and why the graph — not the backend — is
the right owner (frame-global knowledge).

## ⏳ The transient cache (M5.4)
Desc-keyed reuse, lifetime rules, what "no aliasing in v0" costs and how we will measure when
that changes.

## ⏳ Measured overhead (M5.4)
Compile + record cost for the real frame and a synthetic ~100-pass graph, on this repo's dev
server (Release), per "measure before optimize."
