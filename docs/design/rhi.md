# The Render Hardware Interface — design note (M3.1–M3.5)

Companion to `engine/rhi/`. The RHI is the graphics **seam**: the thin, API-agnostic interface every
rendering layer targets, with exactly one backend (Vulkan) underneath. It is the most important
structural bet in the engine after the job system — get the seam right and D3D12/Metal/console
backends are *additions*; get it wrong and the renderer is welded to one API. See
[ADR-0002](../adr/0002-vulkan-first-rhi.md) (why Vulkan-behind-a-seam),
[ADR-0007](../adr/0007-vulkan-backend-bootstrapping.md) (the Vulkan stack),
[ADR-0008](../adr/0008-offline-shader-compilation.md) (shaders),
[ADR-0009](../adr/0009-swapchain-and-presentation.md) (presentation), and
[ADR-0010](../adr/0010-textures-and-descriptors.md) (textures & descriptors).

## The seam, and how it's enforced

`include/rime/rhi/` contains no Vulkan type — not `VkDevice`, not `VkFormat`, nothing. Callers see
`rhi::Device`, `rhi::Format`, opaque handles. The Vulkan backend under `src/vulkan/` is the only code
that includes `<vulkan.h>`, and its dependencies (volk, the headers, VMA) are linked **PRIVATE** to
`rime_rhi`. So a consumer *cannot* include a Vulkan header transitively even if it tried — the build
graph forbids it. This is the same trick `engine/platform` uses for OS headers ("no `#ifdef` upward"),
applied to graphics. The seam is a property of the build, not a rule we remember to follow.

## Resources are handles, not pointers

Every GPU object — buffer, texture, shader, pipeline — is referred to by a generational
`core::Handle` (the M1 slot-map mechanism that `platform::WindowId` also uses), never a raw pointer.
The backend keeps a `SlotMap` per resource kind; a handle is a cheap 8-byte `{index, generation}` id
that detects use-after-free (a destroyed-and-reused slot bumps its generation, so a stale handle is
rejected rather than aliasing the new occupant). This is the data-oriented default the whole engine
shares: systems pass handles around and the backend owns the objects.

A small wrinkle lives at the boundary: the public `BufferHandle` is `Handle<Buffer>` (a phantom tag
that keeps the *public* API type-safe), while the backend stores `SlotMap<VulkanBuffer>` keyed by
`Handle<VulkanBuffer>`. The two are layout-identical, so a single sanctioned helper, `rebrand<>`,
re-tags a handle at the public/backend boundary. The phantom typing still stops a `TextureHandle`
being passed where a `BufferHandle` is wanted.

## Dynamic rendering, not render passes

The backend targets **Vulkan 1.3 dynamic rendering**: a render is `begin_rendering(color target,
load/store, clear)` … draws … `end_rendering()`, with no `VkRenderPass` or `VkFramebuffer` objects to
pre-declare. Pipelines carry their color *format* (`VkPipelineRenderingCreateInfo`) instead of a
render-pass handle. This is dramatically less boilerplate than the classic model and is precisely the
shape the M5 render graph wants — each pass records into a command buffer and declares the resources
it touches. Synchronization uses **synchronization2** (`VkImageMemoryBarrier2`/`vkCmdPipelineBarrier2`),
and — deliberately — the RHI inserts image-layout transitions *inside* the backend, so callers never
touch a barrier. Explicit barrier control is a power-user feature we'll expose when the render graph
needs it, not before.

## The "first pixels" proof, and why it's off-screen

M3's headline is a textured quad on screen, but a CI runner has **no GPU and no display**. The roadmap
rule is "a milestone is done only when its proof *runs*," so the proof must run headlessly. The answer
is the same split M2 used (a null window backend for CI, a real window on dev): render the triangle to
an off-screen `VkImage`, copy it into a host-visible buffer, and **assert on the actual pixels** — the
center is the triangle's red, a corner is the clear color. No surface, no swapchain, no display
needed, so it runs on **lavapipe** (Mesa's software Vulkan) in CI on all three OSes and is
bit-deterministic. The on-screen swapchain path (M3.4) is the dev/headed proof. `tests/rhi/
offscreen_triangle_test` is the CI gate; `samples/01-hello-triangle` runs the identical render and
prints an ASCII preview so a human can see it too. M3's *actual* headline — the textured quad — uses
the very same machinery: `tests/rhi/textured_quad_test` asserts the four R/G/B/Y quadrants of a 2×2
texture pixel by pixel, and `samples/02-textured-quad` runs the identical render (windowed, or an
off-screen colored-letter preview).

## Submission model (M3-simple)

`begin_commands()` → record → `submit_blocking()` submits the work and waits on a fence. One render,
one submit, one wait — the simplest correct model, and exactly right for a one-shot off-screen render.
It is *not* how a real frame loop works: M3.4 adds frames-in-flight (overlapping CPU recording with
GPU execution), paced by swapchain presentation. We build the simple thing first and label it.

## Presentation and the swapchain (M3.4)

Putting the triangle in a window adds one object — `rhi::Swapchain`, created from the `Device` and a
`platform::NativeWindow` — and deliberately *nothing* to the `Device`'s shape: the Device stays
window-agnostic, so the headless off-screen proof keeps running GPU-free in CI. The swapchain is the
only RHI object that owns a `VkSurfaceKHR`; surface extensions are enabled opportunistically (present
on a real GPU/MoltenVK, absent on lavapipe), so presentation is purely additive. See ADR-0009.

The frame loop is the shape the render graph will drive: `acquire_next_image()` hands back a backbuffer
*as an ordinary `TextureHandle`* (so `begin_rendering` and the layout transitions work on it
unchanged), you record a render into it, and `present()` submits with this frame's synchronization and
queues the present. An invalid handle (or a false from `present`) means "out of date" — the window
resized — so the caller calls `recreate()` and continues. Two **frames in flight** overlap CPU and GPU
(per-frame image-available semaphore + in-flight fence; a per-image render-finished semaphore gates the
present); presentation itself paces the loop (FIFO vsync), so there is no manual sleep.

Surface creation is the one place the backend touches an OS windowing type: `surface_vulkan.cpp`
switches on `NativeWindow.system` and calls the matching `vkCreate*SurfaceKHR`, each branch guarded by
a per-OS `VK_USE_PLATFORM_*` macro — the same "add a backend dir/branch, not an `#ifdef` in a header"
discipline `engine/platform` uses. The `NativeWindow` handles stay type-erased (`void*`); only this
file reinterprets them.

## Textures, samplers & descriptors (M3.5)

The textured quad is M3's "done when," and the one new idea it forces is **descriptors** — how a
shader is told *which* resources to read. Three small resources lead up to it, then the descriptor
model ties them to the shader. See ADR-0010.

- **Index buffers.** `bind_index_buffer(buffer, IndexType, offset)` + `draw_indexed(...)` let triangles
  share vertices (a quad is 4 vertices + 6 indices). `IndexType` is `Uint16`/`Uint32` — 16-bit halves
  bandwidth and covers meshes up to ~65k vertices.
- **Texture upload.** `Device::write_texture(handle, data, size)` copies tightly-packed pixels into a
  device-local image **through a staging buffer**, blocking, and leaves it shader-readable — the same
  one-shot model as `submit_blocking`. The image must carry `TransferDst` usage. Batched/streamed
  uploads are the asset pipeline's job (M6).
- **Samplers.** A `Sampler` is *how* an image is read (min/mag `Filter`, `AddressMode` for out-of-range
  UVs), decoupled from the image so one texture can be sampled different ways. The vocabulary is a
  deliberate small subset (`Nearest`/`Linear`, `Repeat`/`ClampToEdge`), grown on demand.

**The descriptor model is intentionally one notch above trivial.** A pipeline opts in with a single
flag — `GraphicsPipelineDesc::sampled_texture = true` — and the backend gives it a descriptor-set
layout of exactly *set 0, binding 0 = one combined image-sampler, fragment stage* (a GLSL `sampler2D`);
a non-sampling pipeline keeps the empty layout from M3.3. At draw time `bind_texture(binding, texture,
sampler)` (after `bind_pipeline`) allocates that set lazily from a small device-owned pool, **caches it
on the pipeline**, and rewrites it *only when the texture/sampler changes* — so a static material never
rewrites a set that may still be in flight on another frame (this is what makes it correct under the
swapchain's two frames-in-flight). A **combined image-sampler** (one descriptor tying an image view +
sampler) is the least machinery that works; separate descriptors and bindless are deferred to where
they pay off — the render graph. The deliberate cost is that a set lives *on the pipeline*, conflating
"material" with "pipeline" — one texture per pipeline, fine for a quad, replaced by per-draw/per-material
sets when the render graph owns binding.

## Deliberate limitations (labeled, per CLAUDE.md)

- **Device owns its instance.** One `Device` == one instance + physical + logical device + allocator.
  A separate `Instance`/adapter-enumeration object (multi-GPU, explicit adapter choice) is a clean
  later seam; not needed for first pixels.
- **Blocking submit for off-screen; frames-in-flight for the window.** `submit_blocking()` is the
  one-shot off-screen path (M3.3); the swapchain (M3.4) overlaps two frames. Command-buffer
  pooling/recycling is still a later optimization — today each submission allocates one.
- **One combined image-sampler per pipeline; no push constants or uniform buffers.** M3.5 adds index
  buffers, texture upload, samplers, and a single-binding descriptor set (above) — but the set is cached
  *per pipeline* (one texture per material), and there are still no push constants, uniform buffers,
  vertex-stage textures, or multiple color attachments. Those arrive with the renderer / render graph (M5).
- **`std::unique_ptr<CommandBuffer>` per submission.** A small allocation per submit; command-buffer
  pooling/recycling is a later optimization, to be made with measurement.
- **Layout tracking is a single field per texture.** Correct for single-threaded M3 recording; the
  render graph will own resource state/lifetime properly.

## Where this goes

The render graph (M5) records its passes through this interface, and the ECS-driven renderer feeds it.
With M3.4 (the swapchain, reaching a window — and finally mapping the M2 Wayland surface) and M3.5 (the
texture/sampler/descriptor model) both in place, **M3 is complete**: everything visual in Rime
ultimately crosses this seam. *Inspired by: the RHI/RDG discipline of UE5, WebGPU's clean resource
model, and the "thin explicit abstraction over one good backend" approach proven by sokol-gfx and wgpu.*
