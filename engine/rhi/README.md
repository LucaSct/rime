# engine/rhi — the Render Hardware Interface (the graphics seam)

`rime::rhi` is the boundary between engine rendering code and the GPU API. Everything above it —
the future render graph, the renderer, samples — targets the **API-agnostic interface** under
`include/rime/rhi/`. Underneath sits one backend (`src/vulkan/`, the Vulkan implementation).

**The one rule:** *no Vulkan (or any backend) type leaks upward.* The public headers never include
a Vulkan header; the Vulkan backend is the only code that does, and its dependencies (volk, the
Vulkan headers, VMA) are linked **PRIVATE**, so nothing that links `rime::rhi` can even transitively
see `<vulkan.h>`. The seam is enforced by the build, not just by review — the same discipline as
platform's "no OS `#ifdef` upward." This is the bet that lets a D3D12/Metal/console backend land
later as an *addition*, not a renderer rewrite. See [ADR-0002](../../docs/adr/0002-vulkan-first-rhi.md),
[ADR-0007](../../docs/adr/0007-vulkan-backend-bootstrapping.md), and
[docs/design/rhi.md](../../docs/design/rhi.md).

Depends only on `core` (handles, log, asserts) and `platform` (the `NativeWindow` it will turn into
a surface when presentation lands). Resources are referred to by generational `core::Handle`s, never
raw pointers.

## Status (built bottom-up, brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| Brick | Provides | State |
| --- | --- | --- |
| M3.1 | RHI seam + Vulkan instance/device/queues/VMA; `Device` factory + `AdapterInfo` | landed |
| M3.2 | offline GLSL→SPIR-V (`rime_add_shaders`) + RHI `Shader` | landed |
| M3.3 | graphics `Pipeline`, command encoder (dynamic rendering), buffers/textures, off-screen render + readback — the **first-pixels** proof | landed |
| M3.4 | swapchain + present from a window (Win/Linux + macOS/MoltenVK) | landed (ADR-0009) |
| M3.5 | textures + samplers + descriptors → the textured quad (M3 "done when") | landed (ADR-0010) |
| +viewer | depth attachment · push constants · 3-D/volume textures · stencil — pulled ahead of M5 to serve the ICEM viewer, adopted by the render graph later | landed (ADRs 0011–0014) |

> *"landed"* = merged and green in CI on all three OSes (Linux/lavapipe with
> `RIME_REQUIRE_VULKAN=1`; macOS/MoltenVK locally). M3 is complete — the whole seam + Vulkan
> backend shipped in PR #2 (2026-07).
>
> **Known gaps before the RHI is renderer-ready** (burned down early in M5, in graph-need
> order): no compute dispatch / storage images, no multiple render targets or blending, no
> MSAA/mipmaps, a single-set/single-binding descriptor model (16-set pool), one queue, a fixed
> two frames in flight, and single-threaded command recording.

## Layout

```
include/rime/rhi/   # API-agnostic public interface (no Vulkan headers here, ever)
  types.hpp         #   enums, PODs, opaque resource handles
  resources.hpp     #   Buffer/Texture/Shader/GraphicsPipeline descriptors
  device.hpp        #   Device: factory + resource creation + submission
  command_buffer.hpp#   CommandBuffer: the recording interface
src/                # backend-agnostic factory glue (device.cpp)
src/vulkan/         # the Vulkan backend — the only code that includes Vulkan headers
```

## Baseline

Vulkan **1.3**, using **dynamic rendering** + **synchronization2** (no `VkRenderPass`/`VkFramebuffer`
objects), **volk** as the loader, and **VMA** for memory. The runtime loader + an ICD (a real GPU
driver, MoltenVK on macOS, or lavapipe in CI) come from the environment, not the build. See ADR-0007.
