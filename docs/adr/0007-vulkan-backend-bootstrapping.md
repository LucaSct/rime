# ADR-0007: Vulkan backend bootstrapping (volk, VMA, Vulkan 1.3)

- Status: Accepted
- Date: 2026-06-18

## Context

Milestone 3 builds the Vulkan backend that sits behind the RHI ([ADR-0002](0002-vulkan-first-rhi.md)).
"Use Vulkan" leaves several bootstrapping decisions open, and they shape every line of the backend:

1. **Loading Vulkan.** Link the loader (`libvulkan`/`vulkan-1`) and call `vk*` directly, or use a
   meta-loader like **volk** that resolves entry points at runtime (and supports per-device dispatch).
2. **GPU memory.** Call `vkAllocateMemory` ourselves, or use **VMA** (Vulkan Memory Allocator), the
   battle-tested sub-allocator already named in the roadmap and `third_party/README.md`.
3. **Feature baseline.** Target old, widely-supported Vulkan 1.0/1.1 with `VkRenderPass`/
   `VkFramebuffer` objects, or the modern **Vulkan 1.3** core: **dynamic rendering** (no render-pass
   objects) + **synchronization2** (a cleaner barrier model).
4. **Validation & where dependencies come from**, and — the hard one — **how the proof runs in CI**,
   where GitHub's runners have **no GPU**, yet the roadmap rule is "a milestone is done only when its
   proof *runs*."

The forces: Rime is power-first (VISION #1) and a teaching codebase (#3); the renderer is the future
render graph's foundation (ROADMAP "seams before features"); and the build must stay reproducible and
CI-gated on all three OSes from M0.

## Decision

**Bring the Vulkan backend up on a modern, lean stack:**

- **volk** is the loader. With `VK_NO_PROTOTYPES`, there are no statically linked `vk*` symbols; volk
  `dlopen()`s the platform loader at runtime and loads instance- then device-level entry points. We
  therefore **link no loader at build time** — only the headers and volk.
- **VMA** owns GPU memory. We never call `vkAllocateMemory` directly; buffers/images are sub-allocated
  by VMA, configured by access pattern (GpuOnly / CpuToGpu / GpuToCpu). VMA is fed volk's function
  pointers (dynamic functions), and its single-header implementation is compiled in one isolated TU
  (`rime_vma`) with relaxed warnings.
- **Vulkan 1.3 is the baseline**: we require `dynamicRendering` + `synchronization2` and use them — no
  `VkRenderPass`/`VkFramebuffer` objects, and barriers via `VkImageMemoryBarrier2`/`vkCmdPipelineBarrier2`.
  This keeps the RHI small and maps cleanly onto the M5 render graph.
- **Validation layers + a debug-utils messenger are on in debug builds** (off when optimized) — the
  same policy as core's assertions — and degrade gracefully if the layers aren't installed.
- **Build dependencies come from Conan** (`vulkan-headers`, `volk`, `vulkan-memory-allocator`), with
  `glslang` as a build-context tool ([ADR-0008](0008-offline-shader-compilation.md)). The **runtime
  loader + ICD are the environment's job**, not Conan's — a real GPU driver on a dev box, **MoltenVK**
  on macOS, and **lavapipe** (Mesa's software Vulkan) on GPU-less CI runners.
- **The CI proof is an off-screen render + pixel readback.** Rendering to an image (not a swapchain)
  needs no display, so it runs on lavapipe headlessly and verifies real pixels deterministically. This
  mirrors M2's split exactly: a null/headless path for CI, the real path (a window/swapchain) on dev.

## Consequences

**Good**
- Smallest modern surface: no loader to ship or link; per-device dispatch via volk; memory handled by
  proven code. The renderer reaches "first pixels" fast and stays clean.
- Dynamic rendering + sync2 remove a whole category of boilerplate (render-pass/framebuffer/subpass
  objects) and are exactly the model the render graph wants — no retrofit later.
- "First pixels" is a **real, green CI gate on all three OSes**, GPU or not, because the proof renders
  off-screen on a software ICD.
- Dependencies are reproducible (Conan), and the dev-vs-CI driver split is the same idea M2 used.

**Costs we accept**
- volk imposes the `VK_NO_PROTOTYPES` discipline and an init step (`volkInitialize`/`volkLoadInstance`/
  `volkLoadDevice`); a missing loader is a graceful "no device," which callers must handle.
- A 1.3 baseline excludes some old/embedded drivers. Acceptable — our targets (modern desktop GPUs,
  MoltenVK, lavapipe) all support it; we revisit only if a real target can't.
- MoltenVK is a portability driver: we must enable the portability enumeration/subset extensions, and
  some features are emulated. Acceptable now (ADR-0002 already accepts the MoltenVK trade-off).
- A software rasterizer is slow; we use it only for correctness in CI, never performance.

## Alternatives considered

- **Link the Vulkan loader directly (no volk).** Simpler init, but we'd ship/locate the loader and
  lose cheap per-device dispatch; volk is tiny and standard. Rejected.
- **Vulkan 1.0/1.1 with render-pass objects.** Maximum driver reach, but more boilerplate now and a
  refactor when the render graph lands. The 1.3 path is both simpler and more future-aligned. Rejected.
- **Hand-rolled GPU allocator.** Re-implements VMA for no gain at this stage; we own memory *policy*
  through VMA's access-pattern API without owning its bugs. Rejected (revisit only with measurement).
- **Vulkan SDK as the dependency source.** The current `scripts/setup` already hints at it, and it
  bundles validation + tools — but it adds a system prerequisite outside the Conan graph. We keep the
  *build* deps in Conan and let the SDK/drivers serve the *runtime* (validation layers, ICDs) instead.
