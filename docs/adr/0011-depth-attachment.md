# ADR-0011: Depth attachment & the depth test

- Status: Accepted
- Date: 2026-06-22

## Context

Through M3 the RHI renders strictly *flat 2-D*: `RenderingInfo` carries a single `ColorAttachment`,
pipelines have no depth state, and `begin_rendering` wires only color. That is all a triangle or a
textured quad needs. The moment anything **3-D** is drawn — the ICEM viewer (its first real consumer),
and the M5 render graph after it — fragments must be resolved by *distance from the camera*, not by
submission order. Without a depth buffer, a nearer triangle submitted before a farther one is painted
over by the farther one; correct opaque 3-D is impossible.

The roadmap originally slated depth for **M5** (it is listed there as "a depth pre-pass"). We are
pulling the *attachment* forward because the viewer needs depth-correct solids now, and the piece is
small and self-contained. The constraint (per CLAUDE.md and the M3-simple precedent) is unchanged:
build the smallest correct thing, label it, and leave the seam for the render graph to own properly.

This ADR covers **depth**. The **stencil** aspect — needed to cap a cross-section so a cut looks solid
— rides on the same attachment and is wired in the cross-section brick; the API is shaped for it here
(a depth *stencil* attachment, a stencil clear value) but the test/state stays disabled until then.

## Decision

**An optional depth attachment on the pass, opt-in depth state on the pipeline — depth off by default.**

- **`RenderingInfo` gains `std::optional<DepthStencilAttachment> depth_stencil`.** Unset → a color-only
  pass, byte-for-byte the old behavior (the M3 triangle/quad are untouched). Set → `begin_rendering`
  transitions the depth image into `DEPTH_ATTACHMENT_OPTIMAL` (through its *depth* aspect) and wires it
  as `VkRenderingInfo::pDepthAttachment`. The attachment carries its own `LoadOp`/`StoreOp`, a
  `clear_depth` (default `1.0` = far plane, the right default for a `Less` test) and a `clear_stencil`
  (carried now, effective when stencil lands).
- **`GraphicsPipelineDesc` gains `depth_test` / `depth_write` / `depth_compare` / `depth_format`**, all
  defaulting to *off*. When `depth_test` is on, the pipeline declares `depthAttachmentFormat` so dynamic
  rendering matches it to a depth-carrying pass (format-compatibility, not a render-pass object —
  ADR-0007). `CompareOp` mirrors `VkCompareOp` one-to-one.
- **We always provide a valid `pDepthStencilState`** (disabled when `depth_test` is off) rather than a
  null pointer, so the pipeline is well-defined on every driver including MoltenVK.
- **`D32Float` is the depth format** for now — a 32-bit float depth, mandatory in Vulkan and supported
  on MoltenVK. A combined depth-*stencil* format (`D32FloatS8Uint`) is added with the stencil brick.
- **Aspect awareness moved into one place.** `aspect_for(VkFormat)` picks depth-vs-color, used by the
  image-view creation and by a new defaulted `aspect` parameter on the shared `transition_image`. No
  call site hard-codes the color aspect anymore.

## Consequences

**Good**
- Depth-correct 3-D is unblocked for the viewer and for M5, through a seam the render graph extends
  rather than replaces (it will add multiple color targets, a stencil aspect, and MSAA on top).
- Proven the M3 way: `tests/rhi/depth_test` renders two overlapping triangles (near-green submitted
  first, far-red second) **twice** — depth on ⇒ center green (nearer wins), depth off ⇒ center red
  (last drawn wins) — an unambiguous, self-controlled pixel proof that runs **GPU-free on lavapipe in
  CI**, exactly like the triangle and quad proofs.
- Zero churn for existing callers: depth is purely additive and off by default.

**Costs we accept**
- **One depth attachment, depth-only, no MSAA, no depth-bounds test.** Enough for opaque 3-D; the rest
  arrives with the render graph / lighting work as real workloads demand it.
- **Stencil is declared but inert** until the cross-section brick — a deliberate, labeled stub.
- **Manual layout bookkeeping** for the depth image (the same single-threaded `VulkanTexture::layout`
  tracking M3 already uses); the render graph owns proper barrier/lifetime tracking later.

## Alternatives considered

- **Wait for the M5 render graph to introduce depth.** Cleanest in theory, but it blocks the viewer
  for an entire milestone for a piece that is small and orthogonal. Pulling just the attachment forward,
  clearly labeled, costs little and the render graph still owns the eventual multi-attachment model.
  Rejected.
- **Make depth always-on (a depth buffer for every pass).** Simpler API (no `optional`), but it forces
  a depth allocation and clear onto the flat-2-D passes (UI, post-process, the existing proofs) that do
  not want one, and changes their behavior. Opt-in keeps 2-D free. Rejected.
- **Add the stencil test now, in the same brick.** Tempting since the attachment supports it, but there
  is no consumer yet and an untested stencil path is dead weight. The format/clear are shaped for it;
  the test lands with the cross-section that needs it. Deferred.
