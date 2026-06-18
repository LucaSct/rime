# The Render Hardware Interface — design note (M3.1–M3.3)

Companion to `engine/rhi/`. The RHI is the graphics **seam**: the thin, API-agnostic interface every
rendering layer targets, with exactly one backend (Vulkan) underneath. It is the most important
structural bet in the engine after the job system — get the seam right and D3D12/Metal/console
backends are *additions*; get it wrong and the renderer is welded to one API. See
[ADR-0002](../adr/0002-vulkan-first-rhi.md) (why Vulkan-behind-a-seam),
[ADR-0007](../adr/0007-vulkan-backend-bootstrapping.md) (the Vulkan stack), and
[ADR-0008](../adr/0008-offline-shader-compilation.md) (shaders).

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
prints an ASCII preview so a human can see it too.

## Submission model (M3-simple)

`begin_commands()` → record → `submit_blocking()` submits the work and waits on a fence. One render,
one submit, one wait — the simplest correct model, and exactly right for a one-shot off-screen render.
It is *not* how a real frame loop works: M3.4 introduces frames-in-flight (overlapping CPU recording
with GPU execution), paced by swapchain presentation. We build the simple thing first and label it.

## Deliberate limitations (labeled, per CLAUDE.md)

- **Device owns its instance.** One `Device` == one instance + physical + logical device + allocator.
  A separate `Instance`/adapter-enumeration object (multi-GPU, explicit adapter choice) is a clean
  later seam; not needed for first pixels.
- **Blocking submit, no pipelining.** See above — frames-in-flight is M3.4.
- **One vertex buffer, empty pipeline layout.** No index buffers, descriptors, or samplers yet; those
  arrive with the textured quad (M3.5). Push constants and multiple attachments come with the renderer.
- **`std::unique_ptr<CommandBuffer>` per submission.** A small allocation per submit; command-buffer
  pooling/recycling is a later optimization, to be made with measurement.
- **Layout tracking is a single field per texture.** Correct for single-threaded M3 recording; the
  render graph will own resource state/lifetime properly.

## Where this goes

The render graph (M5) records its passes through this interface; the ECS-driven renderer feeds it;
M3.5 adds the descriptor/sampler model for textures; M3.4 adds the swapchain so it all reaches a
window (and finally maps the M2 Wayland surface). Everything visual in Rime ultimately crosses this
seam. *Inspired by: the RHI/RDG discipline of UE5, WebGPU's clean resource model, and the "thin
explicit abstraction over one good backend" approach proven by sokol-gfx and wgpu.*
