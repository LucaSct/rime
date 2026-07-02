# ADR-0010: Textures, samplers & the descriptor model (M3.5)

- Status: Accepted
- Date: 2026-06-21

## Context

M3.1–M3.4 render a *flat* triangle: one vertex buffer, an empty pipeline layout, no way for a shader
to read a texture. M3.5 is M3's headline "done when" — a **textured quad** — so the RHI must finally
answer "how does a shader sample a texture?" That is the **descriptor** question, and it is the one
piece of the M3 API genuinely hard to retrofit: descriptors are where bindless, per-material sets, and
the render graph's resource binding will all eventually live. The seam has to be shaped so those grow
*into* it without a rewrite — while staying small enough to draw one quad.

Four smaller pieces ride along, each with a "how minimal?" choice:

1. **Index buffers** — a quad is 4 vertices + 6 indices; trivial, but the encoder needs
   `bind_index_buffer`/`draw_indexed` and an index-width type.
2. **Texture upload** — a device-local image filled from CPU pixels needs a staging copy and a layout
   transition to shader-readable.
3. **Samplers** — *how* a texture is read (filter, addressing), decoupled from the image itself.
4. **The descriptor model itself** — how a texture+sampler reaches the fragment shader, and who owns
   the `VkDescriptorSet` / pool / layout.

Constraint throughout (per CLAUDE.md and the M3-simple precedent set by `submit_blocking`): build the
simplest correct thing, label it, and leave the seam for the render graph (M5) to own properly.

## Decision

**One combined image-sampler, declared by the pipeline, bound per-draw — the minimal descriptor model.**

- **A pipeline declares whether it samples, via a single `bool`.** `GraphicsPipelineDesc::sampled_texture = true` makes the backend create a descriptor-set layout of exactly **set 0, binding 0 = one combined image-sampler, fragment stage** (a GLSL `sampler2D`). A non-sampling pipeline keeps an empty layout, as before. The RHI deliberately does *not* expose a general "describe an arbitrary descriptor-set layout" API yet — that arrives with the material / render-graph layer.
- **Combined image-sampler, not separate image + sampler descriptors.** One descriptor type (`VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`) ties an image view and a sampler in a single binding — the least machinery to get a texture into a shader.
- **`CommandBuffer::bind_texture(binding, texture, sampler)`, after `bind_pipeline`.** The descriptor *set* is allocated lazily on first bind from a device-owned pool, **cached on the pipeline**, and re-written (`vkUpdateDescriptorSets`) **only when the bound texture/sampler changes** — so a static material never rewrites a set that may still be in flight on another frame. It is then bound with `vkCmdBindDescriptorSets`.
- **One fixed-size descriptor pool per `Device`** (16 sets / 16 combined image-samplers), created at device init and freed at teardown. Enough for M3; pool growth, per-frame pools, and recycling are later concerns.
- **`Device::write_texture(handle, data, size)` uploads through a staging buffer, blocking,** and leaves the image in `SHADER_READ_ONLY_OPTIMAL` — the same one-shot M3-simple model as `submit_blocking`. The texture must carry `TransferDst` usage. Batched / streamed uploads belong to the asset pipeline (M6).
- **Index buffers are first-class but minimal:** `bind_index_buffer(buffer, IndexType, offset)` + `draw_indexed(...)`; `IndexType` is `Uint16`/`Uint32` (16-bit halves bandwidth and covers ≤65k-vertex meshes).
- **Samplers are decoupled from textures** (`SamplerDesc{mag/min Filter, AddressMode}`, `create_sampler`), with an intentionally tiny enum subset — `Filter{Nearest, Linear}`, `AddressMode{Repeat, ClampToEdge}` — grown on demand.

## Consequences

**Good**
- M3's headline lands: a textured quad renders through the agnostic RHI, **pixel-verified off-screen** (`tests/rhi/textured_quad_test` asserts the four R/G/B/Y quadrants), so it runs GPU-free on lavapipe in CI exactly like the triangle — and presents in a window on MoltenVK.
- The seam is shaped, not cornered: `sampled_texture` / `bind_texture` are a clean subset of the richer descriptor API the render graph will add; combined image-samplers and the single pool are replaceable *internals*, not public contract.
- The "write only on change" guard makes the cached-on-pipeline set correct under the swapchain's frames-in-flight (ADR-0009): a static material never touches an in-use set.

**Costs we accept**
- **One texture per pipeline.** The set is cached *on the pipeline*, conflating "material" with "pipeline" — fine for one quad, wrong for a real scene. Per-draw / per-material sets (and multiple bindings, vertex-stage textures, uniform buffers, push constants) are the render-graph / material system's job (M5).
- **A fixed 16-set pool with no recycling**, and **blocking single-shot texture upload** — both the deliberate M3-simple model, owned properly later (pool management with the renderer; batched uploads with the asset pipeline).
- **Tiny sampler / format vocabulary** — extended as real assets need it (mipmaps, anisotropy, more address modes and formats).

## Alternatives considered

- **Separate sampled-image + sampler descriptors (or bindless) now.** More flexible and closer to where the engine ends up, but materially more machinery (descriptor indexing, lifetimes) for zero M3 benefit. Combined image-sampler is the smallest correct step; revisit at the render graph. Rejected for now.
- **A full "describe a descriptor-set layout" API in `GraphicsPipelineDesc`.** General, but it bakes a binding model into the public RHI before we know what the material system wants. A single `sampled_texture` bool keeps the surface tiny and honest; the general API lands with its first real consumer. Rejected for now.
- **Per-draw descriptor sets from a per-frame pool.** The correct end state, but it needs the frame / material ownership the render graph provides. Caching one set on the pipeline is enough to draw M3's quad and is clearly labeled temporary. Deferred.
- **A persistent transfer/upload queue for `write_texture`.** Right for streaming, premature for a 2×2 texture. The blocking staging copy mirrors `submit_blocking`; the asset pipeline owns real uploads. Deferred.
