# ADR-0020: Descriptor model v2 — declared binding layouts, transient per-draw sets (M5.1a)

- Status: Accepted
- Date: 2026-07-04

## Context

How a shader is told *which* resources to read is the descriptor model, and M3.5's was
deliberately "one notch above trivial" ([ADR-0010](0010-textures-and-descriptors.md)): a pipeline
could opt into exactly one combined image-sampler at set 0 / binding 0 (`sampled_texture = true`),
the descriptor *set* was allocated once and **cached on the pipeline**, and every set in the
program came from one device-global pool of **16**. ADR-0010 said the quiet part out loud —
"separate descriptors and bindless are deferred to where they pay off — the render graph."
M5 is where that IOU comes due, for three concrete reasons:

- **Per-draw data.** The PBR pipeline (M5.6) reads per-frame constants (camera, lights) and
  per-draw constants (model matrix, material factors) from **uniform buffers** — bindings whose
  *contents or offsets change between draws*. A set cached on the pipeline fundamentally cannot
  express that: the pipeline is "how to draw," but the set answers "with what," and conflating
  them was M3.5's accepted debt.
- **Several bindings per pipeline.** A lit-material shader needs a UBO *and* textures at once;
  the one-binding flag cannot say so.
- **No caps in the way.** A real frame bakes hundreds of sets; a fixed 16-set pool is a cliff,
  and its failure mode (allocation error mid-frame) is the worst kind.

The constraint from the other side: every existing pipeline (samples, viewer, tests) uses the
one-texture model and must keep working — pre-alpha or not, churning 15 call sites to relearn an
API teaches nothing.

## Decision

**Pipelines declare their set-0 binding layout; the encoder bakes pending resource attachments
into a transient descriptor set at each draw, allocated from encoder-owned pools that recycle
through a device free-list once their submission provably finished.**

Four parts:

1. **Declared binding layouts.** `GraphicsPipelineDesc::bindings` is a list of
   `{binding, BindingType, StageMask}` — the *shape* of the pipeline's descriptor set, mirroring
   the shader's `layout(set=0, binding=N)` declarations. `BindingType` is the RHI's small subset:
   UniformBuffer, CombinedImageSampler, and (declared now, wired at M5.2) StorageBuffer /
   StorageImage. `sampled_texture` remains as **sugar** for the old one-texture layout, so every
   existing caller compiles and behaves identically.

2. **Attach, then bake at draw.** `bind_uniform_buffer(binding, buffer, offset, size)` and
   `bind_texture(binding, texture, sampler)` only *record* attachments. At the next draw, if
   anything changed (or the pipeline switched — set compatibility is per layout), the encoder
   allocates one **transient set**, writes every binding the pipeline declared, binds it, and
   forgets it. Sets are never updated in place and never individually freed — so nothing ever
   re-writes a set the GPU may still be reading, by construction. Attachments deliberately
   survive pipeline switches (attach a texture once, draw it through several pipelines); the
   flush validates attachments against the declared layout and reports mismatches as typed RHI
   errors instead of driver mysteries.

3. **Pools recycle on proof of completion.** Each encoder lazily acquires a `VkDescriptorPool`
   from a device **free-list** and chains another on `VK_ERROR_OUT_OF_POOL_MEMORY` — growth, not
   failure; capacity (256 sets/pool) is a performance knob, never a correctness cap. Ownership
   of the pools moves with the submission: `submit_blocking` recycles them right after its fence
   wait; the swapchain parks them per frame-slot and recycles when the slot's fence is waited —
   exactly the schedule its deferred command-buffer free already uses. Recycling is a
   **whole-pool reset** (`vkResetDescriptorPool`), the cheap O(1) way to kill every transient
   set at once.

4. **One set, for now.** Everything lives in set 0. Splitting per-frame / per-material data into
   separate sets (so unchanged sets can persist across draws) is a real optimization — *after*
   the render graph and material system exist to show which bindings actually repeat. Bindless
   is M10-era. Both extend this model without breaking its API.

## Consequences

**Good**

- Per-draw uniform data works — including the classic pattern the proof exercises: one buffer,
  per-draw 256-byte slices, re-bound at a new offset each draw.
- Multi-binding pipelines (UBO + textures) become declarable — the M5.6 PBR prerequisite.
- The 16-set ceiling is gone (the proof bakes 24 sets in one submission; pools chain on demand),
  and steady-state frames allocate no pools at all (free-list reuse).
- Existing code untouched: the sugar keeps the M3.5 surface intact; all 28 pre-existing RHI
  pixel proofs pass unchanged.
- Binding errors (missing attachment, wrong kind) are caught at flush with the pipeline's
  declared layout in hand — named errors at record time beat validation spew at submit time.

**Costs we accept**

- **One set allocation per draw-that-changed-bindings.** Pool allocation is a bump pointer, and
  our draw counts are small; if a profile ever shows set churn dominating, caching identical
  sets is a contained optimization behind `flush_bindings()` — the API doesn't move.
- **Raw Vk objects are captured at bind time** (no re-resolve at flush): destroying a resource
  an un-submitted encoder references is invalid. That rule already existed implicitly; it is now
  explicit in the header.
- **`kMaxBindings = 16` per set** keeps the flush scratch fixed-size and allocation-free; raised
  when a real shader outgrows it, which is far away.

## Alternatives considered

- **Keep the cached-set-per-pipeline model and grow it.** Dies on the first binding that varies
  per draw — updating the cached set in place races the GPU (the M3.5 code dodged this only
  because a static material never re-wrote), and per-draw `vkUpdateDescriptorSets` on a live set
  is exactly the hazard transient sets delete. Rejected.
- **Dynamic uniform-buffer descriptors** (`*_DYNAMIC` + offsets at bind time). Solves only the
  UBO-slice case — not texture swaps, not multi-binding — and adds a parallel offset-passing API
  to teach. Transient sets subsume it: `bind_uniform_buffer` takes the offset directly.
- **Push descriptors** (`VK_KHR_push_descriptor`). Attractive (no pools at all), but an
  extension rather than core 1.3, with per-set size caps, and it removes none of the layout
  machinery — while pools-with-reset are portable and equally simple. Rejected for the seam.
- **A device-global per-frame pool** ("reset at frame start"). Couples every encoder to a global
  frame notion the RHI deliberately doesn't have (offscreen one-shots and swapchain frames
  coexist). Per-encoder pools + a free-list cost the same and make ownership airtight.
- **Jump straight to bindless / `VK_EXT_descriptor_buffer`.** The M10-era end state, requiring
  feature gates and a different shader authoring model — premature while the renderer that would
  exploit it doesn't exist. The declared-layout API survives that future migration.

---

*This ADR is brick **M5.1a**. Proof: `tests/rhi/ubo_test.cpp` — a UBO-driven draw re-bound at a
new slice offset per draw, and 24 transient sets in one command buffer, pixel-verified, GPU-free
on lavapipe. M5.1b (blending, multiple render targets, RGBA16Float) completes the descriptor/
attachment top-up; compute bindings ride M5.2. See [ROADMAP.md](../ROADMAP.md) → M5.*
