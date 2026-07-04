# ADR-0019: The render graph — frame-declared passes, virtual resources, graph-owned barriers (M5.0)

- Status: Accepted
- Date: 2026-07-04

## Context

M5 builds `engine/render`, "where the picture comes from" ([ARCHITECTURE.md](../ARCHITECTURE.md)).
Its "done when" is two claims: **a lit PBR scene draws via the render graph**, and **adding a pass
is easy**. But the graph's real job is bigger than M5: ARCHITECTURE names it *the* structure that
makes UE5-class techniques tractable — dynamic GI, virtual shadow maps, many-lights, virtualized
geometry all arrive at M10 as *passes and resources on this graph*. That makes the graph one of the
engine's **hard-to-retrofit seams** (ROADMAP "seams before features"), and this ADR settles its
shape before any render code lands, so bricks M5.1–M5.9 have a cited decision to build on.

What already exists, and what it implies:

- **The RHI was built for this.** ADR-0007 chose Vulkan 1.3 **dynamic rendering** explicitly so
  passes are `begin_rendering(info)` scopes, not baked `VkRenderPass` objects — the exact shape a
  graph wants to drive. `command_buffer.hpp` records the other half of the bargain: barriers are
  implicit inside the backend *"until the render graph needs explicit control"* — a deferral this
  ADR now calls in.
- **A real multi-pass frame already exists, hand-rolled.** The ICEM viewer composes depth-tested
  meshes, a stencil-marked cross-section cap, and an alpha-tested UI overlay by hand. It works at
  three passes — and it is exactly the kind of code that stops scaling: every new pass must know
  every prior pass's layouts, formats, and lifetimes. The graph exists to make the *frame-global*
  knowledge explicit and checked. (ADR-0016 rule 4 makes "the viewer's frame is expressible as a
  render graph" an M5 acceptance criterion — brick M5.9.)
- **The threading model is job-centric** (ARCHITECTURE §4): render-graph passes are meant to become
  jobs. Whatever we build must not bake in single-threaded or single-queue assumptions, even if v0
  executes serially.
- **The streamer taps the final image.** `engine/stream`'s FrameStreamer reads back an
  already-rendered RHI texture; the graph must be able to *import* such externally-owned targets
  and hand them back in a known state.

The foundational question: **how does a frame get described, scheduled, and synchronized?**

## Decision

**A frame-declared render graph with virtual resources, declared access, and graph-owned
barriers — executed serially on one queue in v0, with the parallel seams kept open.**

Seven parts:

1. **Frame-declared, not retained.** Each frame, render code declares its passes fresh —
   `graph.add_pass(name, kind, declarations, execute λ)` — then the graph **compiles**
   (order, barriers, transients) and **executes**, and is discarded. A frame is *data*, rebuilt as
   cheaply as it is described; toggling an effect, resizing a target, or changing quality tiers is
   just "declare something different next frame" — no retained-graph invalidation protocol. This is
   the UE5 RDG discipline, and its cost (a per-frame compile over dozens of passes) is bounded and
   **measured from day one** (part 6).

2. **Virtual resources; transients owned by the graph.** `graph.create_texture(desc)` /
   `create_buffer(desc)` return lightweight **virtual handles** (`RGTexture`/`RGBuffer`) — no GPU
   memory exists until compile. `graph.import(handle, state)` wraps externally-owned resources (the
   swapchain backbuffer, an offscreen/streamed target, later: persistent history buffers for
   TAA/GI) into the same handle space. At compile, transient descs are satisfied from a
   **cross-frame physical cache** keyed by description — a transient costs its first frame, then is
   recycled. **No memory aliasing in v0**: measure first; aliasing is a compile-step upgrade that
   changes no declaration.

3. **Declared access drives ordering *and* barriers.** A pass declares how it touches each
   resource (ColorWrite, DepthWrite, DepthRead, Sampled, StorageWrite, StorageRead, TransferSrc,
   TransferDst, Present). The graph versions each resource (a write produces version *n+1*; a read
   depends on the version it names), builds the dependency DAG, orders it topologically with
   **declared order as the tiebreak** (deterministic, teachable schedules), and **culls** passes
   that contribute to no imported/output resource. Resource-state tracking then emits **explicit
   transitions through a new public RHI barrier API** (`CommandBuffer::texture_barrier(texture,
   from, to)`-shaped, with RHI-level abstract states the Vulkan backend maps onto
   synchronization2 stages/accesses/layouts). The backend's implicit layout tracking **remains** for
   graph-less code — existing samples/tests/viewer keep working unchanged; the two regimes never
   mix on the same resource within a frame (imported resources declare their entry state and get a
   declared exit state).

4. **Three pass kinds: raster, compute, copy.** A raster pass declares its attachments
   (N color targets + optional depth/stencil) in the pass description; the graph itself opens and
   closes the `begin_rendering`/`end_rendering` scope, so the execute λ only binds pipelines and
   draws. Compute and copy passes execute outside any rendering scope. Multiple render targets are
   first-class in the declaration model from day one (the RHI grows MRT in M5.1b).

5. **Serial, single-queue execution in v0 — parallel seams kept.** Compile yields an ordered pass
   list; v0 records every pass serially into one command buffer and submits once. The *model*
   never assumes this: pass boundaries plus per-pass declared access are exactly the inputs needed
   to (a) record passes in parallel on the JobSystem (one command buffer per pass, submitted in
   compiled order) and (b) schedule independent chains onto an async compute queue at M10. Both
   are measurement-gated upgrades that change no declaration and no pass code.

6. **Timing and legibility built in.** Every pass records begin/end GPU timestamps (M5.3) and a
   debug label (`vkCmdBegin/EndDebugUtilsLabelEXT` when available), so per-pass GPU cost is a
   first-class output of every frame — "measure before optimize" made structural, and RenderDoc
   captures read as the graph reads.

7. **Home and dependencies.** `rime::render::RenderGraph` lives in `engine/render`. The graph core
   depends on `core` + `rhi` only; the ECS-facing scene layer (SceneRenderer, M5.6) lives in the
   same module but is a separate layer above the graph. `engine/render` is removable: the engine
   still builds without it (modularity guardrail #2).

## Consequences

**Good**

- **"Adding a pass is easy" becomes literal**: declare reads/writes + an execute λ; ordering,
  transients, barriers, timing, and labels are the graph's job. The M5.8 sample demonstrates a new
  post-pass in ~10 lines.
- **Synchronization has one owner.** Multi-pass hazards stop being distributed backend guesswork;
  transitions are derived from frame-global knowledge in one auditable place (with validation
  layers as the net). This is the deferral `command_buffer.hpp` promised, paid off.
- **M10 has its home**: GI/VSM/many-lights arrive as passes + resources (including persistent
  history via import and async-compute chains via the kept seam), not as a renderer rewrite.
- **Transient cache** turns per-frame target churn into steady-state reuse.
- **Per-pass GPU ms from the first frame** — every future optimization discussion starts from
  numbers the engine already prints.

**Costs we accept**

- **A per-frame declare+compile cost** — bounded by pass count (dozens), measured in the M5.4
  proof (synthetic ~100-pass graph + the real frame), and cheap relative to what it buys. If a
  profile ever disagrees, caching a compiled schedule keyed by the declaration is a contained
  optimization.
- **v0 leaves GPU/CPU parallelism on the table** (serial record, one queue). Deliberate: correct
  first, parallel when measured — the seams are the point.
- **No aliasing v0** — transient memory is the sum of live cached descs. Fine at M5 scale;
  revisited with real M10 workloads.
- **Declaration discipline**: an execute λ must touch only what its pass declared. Debug asserts
  catch what they can (e.g. barrier-state mismatches); the rest is review discipline until a
  validation mode earns its keep.

## Alternatives considered

- **A retained graph** (build once, patch on change). Saves the per-frame compile, but the things
  M10 wants — per-view pass sets, quality tiers, effect toggles, resolution changes — are exactly
  what makes retained graphs sprout invalidation protocols that cost more than the compile they
  save. UE went frame-declared (RDG) for the same reason. Rejected.
- **Keep hand-rolling passes** (the viewer pattern, scaled up). Works at 3 passes; at 10+, every
  pass author must hold every other pass's layouts/lifetimes in their head — the O(N²) knowledge
  problem the graph exists to delete. Rejected: M5 exists to replace this.
- **Barriers stay implicit in the backend.** The backend sees one command at a time; it cannot know
  that a texture written now is sampled five passes later, so it must guess conservatively (stalls)
  or track fragile heuristics. The graph has the frame-global view; sync belongs where the
  knowledge lives. Rejected (for graph-driven frames; implicit tracking stays for simple code).
- **Parallel recording from day one.** Maximum throughput on paper; in practice a large backend
  surface (per-thread command pools, per-pass buffers, submission ordering) built before a single
  lit pixel exists, against an unmeasured need. Deferred behind kept seams.
- **Adopt an existing framegraph library.** The concepts are small and well-documented; every
  candidate binds to its own RHI-shaped abstractions and would sit *on* our seam rather than behind
  it — and the codebase is a textbook (VISION #3): this is a chapter we must own. Rejected.

---

*This ADR is brick **M5.0**. The code lands next, bottom-up: **M5.1** RHI descriptor model v2 +
uniform buffers, blending, MRT, RGBA16F (ADR-0020) · **M5.2** RHI compute pipelines + storage
resources (ADR-0021) · **M5.3** RHI mipmaps/anisotropy + timestamps/debug labels · **M5.4** the
render graph v0 itself (this design, executable) · **M5.5** the scene layer — camera graduation,
mesh/material registries, ECS render components · **M5.6** the PBR forward pipeline + derivation
(ADR-0022) · **M5.7** `engine/app`'s fixed-tick frame loop (ADR-0023) · **M5.8** the proof samples
`06-render-graph` + `07-first-light` · **M5.9** the ADR-0016 rule-4 acceptance test (the viewer's
frame as a graph). See [ROADMAP.md](../ROADMAP.md) → M5.*
