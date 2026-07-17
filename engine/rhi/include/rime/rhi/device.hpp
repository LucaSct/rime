// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "rime/rhi/command_buffer.hpp"
#include "rime/rhi/resources.hpp"
#include "rime/rhi/swapchain.hpp"
#include "rime/rhi/types.hpp"

// The Device is the root of the RHI: it owns the GPU connection and is the factory for every
// resource and for command buffers. The engine creates one Device and talks only to this interface;
// the concrete VulkanDevice lives entirely under src/vulkan/. (An "Instance"/multi-GPU split is a
// natural later seam; for first pixels, one Device that owns its instance is simpler and enough.)
namespace rime::rhi {

// Validation layers cost performance but catch API misuse early, so we default them on in
// debug-style builds and off when optimized — the same policy as core's assertions. A caller can
// always override per-device.
#if defined(NDEBUG)
inline constexpr bool kValidationDefault = false;
#else
inline constexpr bool kValidationDefault = true;
#endif

struct DeviceDesc {
    std::string_view app_name = "Rime";
    bool enable_validation = kValidationDefault;
    // No window/surface here: M3.1–M3.3 render off-screen (headless), which is what lets the proof
    // run on a software GPU in CI. Presentation (a swapchain built from platform::NativeWindow)
    // arrives in M3.4 as a separate object created from the Device.
};

class Device {
public:
    virtual ~Device() = default;

    // The GPU we ended up on (logged at startup; handy in tests and bug reports).
    [[nodiscard]] virtual const AdapterInfo& adapter() const = 0;

    // ── Resource creation ──────────────────────────────────────────────────────────────────
    // Each returns a handle; on failure the handle is invalid (is_valid() == false) and the cause
    // is logged. No exceptions — the engine forbids them on these paths (CLAUDE.md).
    [[nodiscard]] virtual BufferHandle create_buffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual TextureHandle create_texture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual ShaderHandle create_shader(const ShaderDesc& desc) = 0;
    [[nodiscard]] virtual PipelineHandle
    create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
    // A compute pipeline shares the PipelineHandle space with graphics — one handle type, one
    // destroy() — and records which bind point it targets; binding it through the wrong call
    // (bind_pipeline vs bind_compute_pipeline) is caught at record time. See ADR-0021.
    [[nodiscard]] virtual PipelineHandle
    create_compute_pipeline(const ComputePipelineDesc& desc) = 0;
    [[nodiscard]] virtual SamplerHandle create_sampler(const SamplerDesc& desc) = 0;

    // Destruction is explicit and overloaded per handle type. Destroying an invalid/stale handle is
    // a no-op. (RAII wrappers can be layered on top later; the primitive stays explicit.)
    virtual void destroy(BufferHandle handle) = 0;
    virtual void destroy(TextureHandle handle) = 0;
    virtual void destroy(ShaderHandle handle) = 0;
    virtual void destroy(PipelineHandle handle) = 0;
    virtual void destroy(SamplerHandle handle) = 0;

    // ── Host <-> device data transfer (host-visible buffers) ───────────────────────────────
    // write_buffer copies CPU bytes into a host-visible buffer; read_buffer copies them back out
    // (used to inspect the readback buffer in the proof). Both handle the non-coherent-memory
    // flush/invalidate so the data is actually visible. For GpuOnly buffers these assert.
    virtual void write_buffer(BufferHandle handle,
                              const void* data,
                              std::size_t size,
                              std::size_t offset = 0) = 0;
    virtual void
    read_buffer(BufferHandle handle, void* dst, std::size_t size, std::size_t offset = 0) = 0;

    // Upload pixels into a texture (which must have TransferDst usage). Copies `size` bytes —
    // tightly packed, in the texture's format, covering its full extent — through a staging buffer
    // and leaves the image in a shader-readable layout. One-shot and blocking (the M3-simple model,
    // like submit_blocking); batched/streamed uploads arrive with the renderer and asset pipeline.
    virtual void write_texture(TextureHandle handle, const void* data, std::size_t size) = 0;

    // Upload a full, PRE-GENERATED mip chain: `levels[i]` is the tightly-packed pixels for mip
    // level i (level 0 = full extent; each subsequent level halved, floor 1), in the texture's
    // format. Unlike write_texture — which fills level 0 and GPU-downsamples the rest — this copies
    // each level's bytes verbatim: the path for cooked textures whose mips were generated offline
    // in linear space (M6.3, ADR-0024). `levels.size()` must equal the texture's mip_levels; the
    // image is left shader-readable. Also one-shot and blocking.
    virtual void write_texture_mips(TextureHandle handle, std::span<const MipData> levels) = 0;

    // ── Command submission ─────────────────────────────────────────────────────────────────
    // begin_commands hands out a fresh encoder. record into it, then submit_blocking submits the
    // work and waits for the GPU to finish — the simplest correct model, perfect for the one-shot
    // offscreen render in the M3 proof. Frames-in-flight pipelining (overlapping CPU and GPU)
    // arrives with the swapchain in M3.4, where presentation sets the frame cadence.
    [[nodiscard]] virtual std::unique_ptr<CommandBuffer> begin_commands() = 0;
    virtual void submit_blocking(CommandBuffer& commands) = 0;

    // Asynchronous submission — the non-blocking counterpart to submit_blocking, and the seam the
    // frame tap (engine/stream) rides to hide the glass-to-CPU readback stall (ADR-0030, s1.1).
    // submit() hands the recorded work to the GPU and returns immediately with a SubmitTicket;
    // unlike submit_blocking it takes OWNERSHIP of the command buffer, because the backend must
    // keep it — and the transient descriptor pools it baked — alive until the GPU is done, which
    // the caller's scope no longer decides. Poll the ticket with is_complete() or block with
    // wait(); whichever first observes completion reclaims the command buffer + pools.
    [[nodiscard]] virtual SubmitTicket submit(std::unique_ptr<CommandBuffer> commands) = 0;

    // Has the work behind `ticket` finished on the GPU? Non-blocking (a fence poll). Reclaims the
    // submission's resources on the first call that sees it complete. An invalid, unknown, or
    // already-reclaimed ticket returns true — there is nothing left in flight.
    [[nodiscard]] virtual bool is_complete(SubmitTicket ticket) = 0;

    // Block until `ticket`'s work finishes, then reclaim it. A no-op for an invalid/unknown ticket.
    virtual void wait(SubmitTicket ticket) = 0;

    // Block until the GPU is idle. Used before tearing down resources.
    virtual void wait_idle() = 0;

    // ── Presentation (M3.4) ────────────────────────────────────────────────────────────────
    // Create a swapchain that presents to a platform window (built from its NativeWindow handles).
    // Returns nullptr if the device has no surface/swapchain support — e.g. a headless device on a
    // software ICD, where the off-screen path is used instead. The Device stays window-agnostic;
    // the returned Swapchain is the only object that owns a surface. See swapchain.hpp / ADR-0009.
    [[nodiscard]] virtual std::unique_ptr<Swapchain>
    create_swapchain(const SwapchainDesc& desc) = 0;

protected:
    Device() = default; // construct only through create_device()

public:
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
};

// Create the RHI device. Returns nullptr if no usable GPU/driver is available — notably when there
// is no Vulkan loader or ICD installed (a headless machine with no software driver). Callers that
// must degrade gracefully (tests, tools) should check for null rather than assume success.
[[nodiscard]] std::unique_ptr<Device> create_device(const DeviceDesc& desc);

} // namespace rime::rhi
