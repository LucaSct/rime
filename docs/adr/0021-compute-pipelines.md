# ADR-0021: Compute pipelines — one handle space, shared bindings, blunt barriers for now (M5.2)

- Status: Accepted
- Date: 2026-07-04

## Context

`ShaderStage::Compute` has sat in the RHI since M3 with nothing behind it. M5 is where that ends,
for reasons bigger than M5 itself: the render graph declares **compute** as one of its three pass
kinds ([ADR-0019](0019-render-graph.md)), and nearly everything on the M10 slate — GI probes and
radiance caches, light binning/culling, virtualized-geometry culling — is compute-shaped. The
2026-07-02 plan put compute *first* in the RHI burn-down ("unblocks every GPU-driven ambition").
[ADR-0020](0020-descriptor-model-v2.md) already declared the storage binding types
(StorageBuffer / StorageImage) precisely so compute would not force a second binding-model
migration.

What compute needs that the RHI lacks: a pipeline kind with no fixed-function state, a dispatch
call, read-write (storage) resource access, the image layout storage access requires (GENERAL),
and *some* answer to synchronization — a dispatch's writes must be visible to whatever consumes
them, and the render graph that will own synchronization properly is still two bricks away.

## Decision

**Compute pipelines share the graphics pipeline's handle space and binding model; dispatches run
on the one queue, outside rendering scopes, followed by a deliberately blunt full barrier until
the render graph owns synchronization.**

Five parts:

1. **One `PipelineHandle` for both kinds.** `create_compute_pipeline(ComputePipelineDesc)` — a
   shader + the same declared binding layout + a push-constant budget, nothing else (compute has
   no fixed-function state) — returns the same handle type graphics uses; the backend records the
   pipeline's bind point and catches wrong-call misuse (`bind_pipeline` vs
   `bind_compute_pipeline`) at record time with a typed error. One handle type, one `destroy`,
   one SlotMap; the type system stays small.

2. **The binding model is shared, deliberately.** `bind_storage_buffer` / `bind_storage_image`
   join `bind_uniform_buffer` / `bind_texture` as attach-then-bake calls (ADR-0020); pending
   attachments survive across bind points, so a buffer attached once can feed a dispatch and then
   the draw that renders its results. `flush_bindings` bakes into whichever bind point the
   current pipeline declares.

3. **Storage images live in GENERAL, put there by the backend.** `bind_storage_image` transitions
   the image to the general layout at bind time (the M3 philosophy — the backend puts an image
   where its declared use needs it — holds until ADR-0019 hands barriers to the graph). Inside a
   `begin/end_rendering` scope barriers are illegal, so binding a not-yet-GENERAL storage image
   there is a typed error. Sampling an image that *lives* in GENERAL (one a dispatch just wrote)
   is honored by capturing the layout at `bind_texture` time — valid Vulkan, minus an "optimal"
   hint that matters on real GPUs and not at all on lavapipe; precise layout scheduling is
   exactly what the graph will own.

4. **One conservative barrier after every dispatch.** `dispatch()` records a full memory barrier
   (compute-shader writes → all subsequent stages/accesses). Blunt on purpose: it makes every
   dispatch→draw, dispatch→copy, and dispatch→dispatch chain *correct* with zero caller
   ceremony, and it is precisely the over-synchronization ADR-0019's declared-access barriers
   will replace at M5.4. Correctness first; precision when the owner with frame-global knowledge
   exists.

5. **One queue.** Dispatches share the graphics queue. Async compute (a second queue racing
   graphics) is an M10-era upgrade; nothing in this API names a queue, so it arrives as backend
   scheduling, not an API break.

## Consequences

**Good**

- The whole GPGPU loop exists: create → bind → dispatch → read back, proven element-for-element,
  plus the compute→graphics handoff (storage image written by a dispatch, sampled by a draw in
  the same command buffer).
- ADR-0019's compute pass kind has a real backend to drive at M5.4.
- One binding model to learn and one flush path to maintain across both pipeline kinds.
- No new enum/API migration later: storage types were declared at M5.1a and are now live.

**Costs we accept**

- **Over-synchronization per dispatch** — the blanket barrier serializes against everything
  after it. Temporary by design (M5.4), and irrelevant at current workloads; the graph's
  per-pass timestamps will show exactly what precision buys when it lands.
- **GENERAL-layout sampling** for compute-written images skips the read-optimized layout on
  hardware that cares. Same remedy, same owner: the graph.
- **No `dispatch_indirect`** — GPU-driven dispatch arrives with its first consumer (M10-era
  culling), a small additive call.

## Alternatives considered

- **A distinct `ComputePipelineHandle` type.** More compile-time safety, but a parallel
  destroy/lookup/storage surface for a mistake (binding through the wrong call) the backend
  already catches at record time with a clear message. Rejected: one handle space, smaller API.
- **Expose barriers to callers now.** The honest long-term shape — but the caller with
  frame-global knowledge (the render graph) is two bricks away, and a public barrier API
  designed before its real consumer exists is how APIs grow warts. The blunt barrier keeps
  callers oblivious and the seam clean; ADR-0019 already owns the end state.
- **Async compute queue now.** No workload exists to prove it, and multi-queue submission
  reshapes swapchain synchronization. The API deliberately never names a queue, so the seam is
  already there. Deferred to M10-era.
- **Storage-image layout as caller responsibility.** Pushes Vulkan's sharpest edge (layout
  mismatches = silent corruption on real drivers) onto every kernel author for zero gain while
  the backend still owns every other transition. Rejected for v0.

---

*This ADR is brick **M5.2**. Proof: `tests/rhi/compute_test.cpp` — a 1024-element storage-buffer
fill verified value-for-value, and a compute-written storage image sampled by a draw in the same
submission, pixel-verified; GPU-free on lavapipe. Next: **M5.3** mipmaps/anisotropy +
timestamps/debug labels, then **M5.4** — the render graph that takes ownership of the barriers
this ADR left blunt. See [ROADMAP.md](../ROADMAP.md) → M5.*
