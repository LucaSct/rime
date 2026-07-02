# ADR-0012: Push constants for small per-draw data

- Status: Accepted
- Date: 2026-06-22

## Context

[ADR-0010](0010-textures-and-descriptors.md) shipped the minimal descriptor model — one combined
image-sampler — and explicitly **deferred** uniform buffers and push constants "to the render graph /
material system." That was right for a static textured quad. But the ICEM viewer's very first lit mesh
needs one thing that quad never did: a **model-view-projection matrix on the GPU that changes every
frame** as the camera orbits. There is currently no way to get *any* per-draw numeric data to a shader
(the descriptor path carries a texture, nothing else).

The options are push constants or uniform buffers. Uniform buffers are the general answer but drag in
buffer lifetime, per-frame versioning (under frames-in-flight, ADR-0009), and a second descriptor type
— real machinery the render graph should own. Push constants are a tiny, self-contained Vulkan feature
purpose-built for exactly this: a small block of bytes baked into the command stream, no buffer, no
descriptor set. An MVP matrix is 64 bytes, well under the 128-byte minimum every Vulkan device
guarantees.

## Decision

**Add push constants as the per-draw fast path; keep uniform buffers for the render graph.**

- **`GraphicsPipelineDesc::push_constant_size` (bytes, default 0).** Non-zero makes the pipeline layout
  declare one push-constant range `[0, size)` visible to **both** the vertex and fragment stages (so the
  same block can feed transforms in the vertex shader and, later, a clip plane or colormap range in the
  fragment shader). The pipeline records the range's stage mask so the encoder can replay it.
- **`CommandBuffer::push_constants(data, size, offset = 0)`**, called after `bind_pipeline`, maps to
  `vkCmdPushConstants` with the bound pipeline's layout and stage mask. It errors loudly if the bound
  pipeline declared no push constants.
- **Both stages, one range.** A single range visible to vertex+fragment is the least surface that covers
  the viewer's needs; per-stage ranges and multiple ranges are added only if a real case wants them.
- **Stay ≤128 bytes** by convention (the portable guarantee); larger or shared data waits for uniform
  buffers.

## Consequences

**Good**
- The lit mesh renderer (B1) can hand the GPU its MVP each frame with no buffer management, and the
  cross-section (B2) and field colormaps (C) get a ready home for their few parameters.
- Proven the M3 way: `tests/rhi/pushconst_test` draws a full-screen triangle whose color *is* a push
  constant, into two targets with two colors in one command buffer, and asserts each — proving the data
  reaches the shader and changes per draw. Off-screen + readback, GPU-free on lavapipe in CI.
- Purely additive: `push_constant_size = 0` is the default, so every existing pipeline is unchanged.

**Costs we accept**
- **128-byte budget.** Fine for an MVP (+ a plane or a couple of scalars); anything bigger needs uniform
  buffers, deliberately deferred.
- **One range, both stages always.** Marginally over-broad (the vertex shader "sees" fragment-only
  data), but simpler than per-stage ranges and harmless.

## Alternatives considered

- **Uniform buffers now.** The general mechanism, but it needs per-frame buffer versioning and a second
  descriptor type — the render graph's job (M5). Push constants deliver the viewer's per-draw MVP with a
  fraction of the machinery; uniform buffers land with their first larger/shared consumer. Deferred
  (revisits ADR-0010's deferral for push constants specifically).
- **CPU-transform vertices and re-upload each frame.** Avoids any RHI change but re-streams the whole
  mesh per frame and pushes transform work onto the CPU — against the engine's data-oriented, GPU-does-
  transforms grain, and a poor example in a codebase meant to be read. Rejected.
