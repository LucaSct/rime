// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <memory>
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
    [[nodiscard]] virtual PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) =
        0;

    // Destruction is explicit and overloaded per handle type. Destroying an invalid/stale handle is
    // a no-op. (RAII wrappers can be layered on top later; the primitive stays explicit.)
    virtual void destroy(BufferHandle handle) = 0;
    virtual void destroy(TextureHandle handle) = 0;
    virtual void destroy(ShaderHandle handle) = 0;
    virtual void destroy(PipelineHandle handle) = 0;

    // ── Host <-> device data transfer (host-visible buffers) ───────────────────────────────
    // write_buffer copies CPU bytes into a host-visible buffer; read_buffer copies them back out
    // (used to inspect the readback buffer in the proof). Both handle the non-coherent-memory
    // flush/invalidate so the data is actually visible. For GpuOnly buffers these assert.
    virtual void write_buffer(BufferHandle handle,
                              const void* data,
                              std::size_t size,
                              std::size_t offset = 0) = 0;
    virtual void read_buffer(BufferHandle handle,
                             void* dst,
                             std::size_t size,
                             std::size_t offset = 0) = 0;

    // ── Command submission ─────────────────────────────────────────────────────────────────
    // begin_commands hands out a fresh encoder. record into it, then submit_blocking submits the
    // work and waits for the GPU to finish — the simplest correct model, perfect for the one-shot
    // offscreen render in the M3 proof. Frames-in-flight pipelining (overlapping CPU and GPU)
    // arrives with the swapchain in M3.4, where presentation sets the frame cadence.
    [[nodiscard]] virtual std::unique_ptr<CommandBuffer> begin_commands() = 0;
    virtual void submit_blocking(CommandBuffer& commands) = 0;

    // Block until the GPU is idle. Used before tearing down resources.
    virtual void wait_idle() = 0;

    // ── Presentation (M3.4) ────────────────────────────────────────────────────────────────
    // Create a swapchain that presents to a platform window (built from its NativeWindow handles).
    // Returns nullptr if the device has no surface/swapchain support — e.g. a headless device on a
    // software ICD, where the off-screen path is used instead. The Device stays window-agnostic; the
    // returned Swapchain is the only object that owns a surface. See swapchain.hpp / ADR-0009.
    [[nodiscard]] virtual std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) = 0;

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
