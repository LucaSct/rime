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
| M5.1a | **descriptor model v2** — declared binding layouts (`GraphicsPipelineDesc::bindings`), `bind_uniform_buffer`, transient per-draw sets from recycled grow-on-demand pools (the 16-set cap is gone; `sampled_texture` stays as sugar) | landed (ADR-0020) |
| M5.1b | **blending** (`BlendMode` presets: Alpha "over" / Additive) · **multiple render targets** (`color_formats` + `RenderingInfo::colors`, ≤ 8) · **`Format::RGBA16Float`** — the HDR scene target that keeps radiance > 1.0 for the tonemap pass | landed |
| M5.2 | **compute** — `create_compute_pipeline` + `bind_compute_pipeline` + `dispatch`, storage buffers/images (`bind_storage_buffer` / `bind_storage_image`, GENERAL-layout handling), conservative post-dispatch barrier until the graph owns sync | landed (ADR-0021) |
| M5.3 | **mipmaps** (`TextureDesc::mip_levels`, blit-generated chains) + **anisotropic sampling** (`SamplerDesc::mip_filter`/`max_anisotropy`, feature-gated) · **GPU timestamps** (`write_timestamp`/`read_timestamps`, ns) · **debug labels + object names** (`begin/end_debug_label`, `debug_name` → the driver, guarded on VK_EXT_debug_utils) | landed |
| M6.3 | **per-mip upload** (`write_texture_mips(handle, levels)`, `MipData`) — uploads a cooked, pre-generated mip chain verbatim (one buffer→image copy per level, no blit), so gamma-correct offline mips reach the GPU unregenerated | landed |

> *"landed"* = merged and green in CI on all three OSes (Linux/lavapipe with
> `RIME_REQUIRE_VULKAN=1`; macOS/MoltenVK locally). M3 is complete — the whole seam + Vulkan
> backend shipped in PR #2 (2026-07).
>
> **Known gaps before the RHI is renderer-ready** (remaining after the M5.1–M5.3 top-ups): no
> MSAA, one queue, a fixed two frames in flight, and single-threaded command recording — all
> deliberate, with the seams kept by the render graph (ADR-0019). The RHI is now
> **renderer-ready for M5.4**.

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

**Portability implementations.** When a device exposes `VK_KHR_portability_subset` (MoltenVK does),
`create_logical_device` queries `VkPhysicalDevicePortabilitySubsetFeaturesKHR` and enables back every
capability the device reported. This is not optional politeness: the extension's features start off
like any other, so naming the extension alone opts into its *restrictions* while leaving its
*capabilities* disabled — and using a disabled one is undefined behaviour that no validation layer
flags. `mutableComparisonSamplers` is the sharp edge (depth-compare/shadow samplers); without it
MoltenVK degrades every `sampler2DShadow` fetch to `compare_func::never`, so shadow maps silently
read as *fully occluded* on macOS while lavapipe — not a portability driver, so none of this applies
— stays green. Lesson: **CI's software ICD cannot test the portability path at all.**

Enabling that capability was necessary but, on *some* Metal devices (e.g. Intel Iris/macOS 13),
**not sufficient**: the m10.1/m10.2 shadow maps render depth into a *layered* depth array and sample
it back in the forward pass, and there the sampled depth reads 0 regardless of the compare — hardware
`sampler2DArrayShadow` *and* a manual `sampler2DArray` raw-depth read both come back fully occluded,
even though every Vulkan detail (2D-array sample view, per-layer render views, depth aspect, layouts,
usage) is correct and identical to the green lavapipe/native path. This is a driver limitation in the
dynamic-rendering-depth-then-sample path, not an engine bug. Until the macOS shadow path is reworked
(a Metal-sampleable target is the likely fix), `AdapterInfo::portability` is surfaced so the GPU
shadow proofs skip on portability devices rather than fail — the shadow maths stays proven on CI.
