# ADR-0014: Stencil state (for the cross-section cap)

- Status: Accepted
- Date: 2026-06-27

## Context

[ADR-0011](0011-depth-attachment.md) added a depth attachment and depth test; the comment in the
pipeline's depth/stencil state literally said "stencil stays disabled until the cross-section brick
needs it." That brick is here. The viewer cuts a part with a clip plane (B2) and reveals the interior,
but the **cut face is hollow** — you see through to the back walls. To make the section read as **sawn
solid** (and to paint the computed field on the cut face), the viewer must fill the section plane
exactly where it passes through material. The standard, mesh-exact way to do that in real time is the
**stencil buffer**: in one pass count surfaces along each view ray (back faces +1, front faces −1, with
the front of the solid clipped away); where the count is non-zero the plane is inside the solid, so a
following draw fills those pixels. That needs stencil state the RHI does not yet expose.

Stencil is broadly useful beyond capping (outlines, decals, portals, masked UI), so it earns a first-
class — if minimal — place in the RHI now.

## Decision

**Add a minimal, two-sided stencil to the pipeline + a stencil-capable depth-stencil target.**

- **`Format::D32FloatS8`** (`VK_FORMAT_D32_SFLOAT_S8_UINT`) — a combined depth+stencil target. Chosen
  over D24S8 because it is supported on MoltenVK (our macOS path) and lavapipe alike, and keeps 32-bit
  depth. `aspect_for` now returns depth+stencil for combined formats, and a single image view serves
  **both** the depth and stencil attachments in dynamic rendering.
- **`GraphicsPipelineDesc` stencil fields:** `stencil_test`, a **two-sided** `stencil_front` /
  `stencil_back` (`StencilFace` = fail / depth-fail / pass `StencilOp` + `CompareOp`), and baked
  `stencil_read_mask` / `stencil_write_mask` / `stencil_reference`. Two-sided is the point: with culling
  off, **one draw** can increment on back faces and decrement on front faces — the cap's surface count.
- **`StencilOp`** enum: Keep, Zero, Replace, IncrementWrap, DecrementWrap, Invert (the subset capping +
  common effects need).
- **`color_write` (default true).** `false` masks colour (`colorWriteMask = 0`) so the stencil-marking
  pass updates only stencil, leaving the rendered image intact.
- **Attachment wiring.** `begin_rendering` binds a D32FloatS8 target as both the depth and stencil
  attachment (one view), transitions it through its depth+stencil aspect into
  `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, and clears stencil via the existing `clear_stencil`. A pipeline
  declares the stencil attachment format whenever its `depth_format` carries stencil, so every pipeline
  in a stencil pass agrees with the pass. **Reference/masks are static** (baked into the pipeline) for
  now — dynamic stencil reference is a later addition if a use case wants it.

## Consequences

**Good**
- The cross-section **solid cap** (B2b·V) is unblocked: a marking pass counts where the plane is inside
  the solid, and a cap quad fills it — coloured by the field (folding in C1's deferred flat slice).
- Reusable: outlines/decals/portals get the same machinery later.
- Proven the M3 way: `tests/rhi/stencil_cap_test` writes stencil = 1 on a centred triangle (colour
  masked off) then fills green only where stencil == 1; the centre is green, a corner stays the clear
  colour. Off-screen + readback, GPU-free on lavapipe in CI. All 10 rhi tests pass.
- Additive: `stencil_test` defaults off and `color_write` defaults on, so every existing pipeline and
  the depth-only path (D32Float, no stencil attachment) are unchanged.

**Costs we accept**
- **Static reference/masks.** One reference per pipeline; an effect needing per-draw stencil reference
  will add `VK_DYNAMIC_STATE_STENCIL_REFERENCE` then. Fine for the cap (reference 0/1).
- **One depth-stencil format.** D32FloatS8 only; D24S8 (smaller) is a one-line addition if a target
  prefers it.

## Alternatives considered

- **Capping by the field's validity volume (no stencil).** The `.icef` carries a validity mask; a cap
  quad could discard where validity < 0.5. Zero RHI work and reuses C1 — but it only works when a field
  is loaded and the cap edge is field-grid resolution (smooth but not mesh-crisp). Stencil gives a
  mesh-exact cap for any part, with or without a field. Rejected as the primary mechanism (the field
  still *colours* the stencil cap).
- **Depth-peeling / order-independent counting.** More general but far heavier; unnecessary for a single
  cutting plane. Rejected.
