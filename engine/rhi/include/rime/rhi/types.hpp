// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <string>

#include "rime/core/containers/handle.hpp"

// The vocabulary of the Render Hardware Interface: the enums, plain-data descriptors, and opaque
// resource handles that every layer above the RHI speaks. This header — and the whole
// rime/rhi/ interface — deliberately contains **no Vulkan (or any graphics-API) types**. That is
// the entire point of the seam (ADR-0002): the renderer targets these abstract types, and the one
// backend under src/vulkan/ translates them to VkFormat, VkBufferUsageFlags, and friends. Swapping
// in a D3D12/Metal backend later means writing a new translator, not touching a single caller.
namespace rime::rhi {

// ── Resource handles ──────────────────────────────────────────────────────────────────────────
// GPU resources are referred to by generational handles (core::Handle), never raw pointers. A
// handle is a cheap, copyable 8-byte id that the backend resolves to its real object through a
// SlotMap; if a resource is destroyed and its slot reused, stale handles are detected rather than
// silently aliasing the new occupant. This is the same data-oriented model platform::WindowId and
// the ECS use — the engine passes handles around, the backend owns the objects.
//
// The tag types below exist only to make the handles distinct in the type system (a BufferHandle
// cannot be passed where a TextureHandle is wanted). They are never defined — purely phantom.
struct Buffer;
struct Texture;
struct Shader;
struct Pipeline;

using BufferHandle = core::Handle<Buffer>;
using TextureHandle = core::Handle<Texture>;
using ShaderHandle = core::Handle<Shader>;
using PipelineHandle = core::Handle<Pipeline>;

// ── Small geometric PODs ────────────────────────────────────────────────────────────────────
// rhi has its own Extent2D (rather than reusing platform::Extent2D) so the graphics seam owns its
// vocabulary and does not drag a platform dependency into every consumer of a size.
struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// A viewport in framebuffer pixels. `min_depth`/`max_depth` map NDC z into the depth range
// (usually 0→1). y-down matches our framebuffer convention; the backend flips if a target needs it.
struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

struct Rect2D {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ClearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// ── Enumerations ────────────────────────────────────────────────────────────────────────────

// Pixel/vertex-attribute formats. A small, intentional subset — we add formats as real workloads
// demand them, rather than mirroring Vulkan's hundreds. Names read as channels + bit depth + type.
enum class Format : std::uint32_t {
    Undefined = 0,
    R8Unorm,
    RGBA8Unorm, // the offscreen color target + readback format in the M3 proof
    RGBA8Srgb,
    BGRA8Unorm, // the common swapchain format (arrives with presentation in M3.4)
    BGRA8Srgb,
    RG32Float,    // vec2 vertex attribute (e.g. UVs)
    RGB32Float,   // vec3 vertex attribute (positions, colors)
    RGBA32Float,  // vec4 vertex attribute
    D32Float,     // depth (arrives when we add a depth pre-pass in M5)
};

// What a buffer can be used for. Bit flags: OR them together (see RIME_RHI_FLAGS below). The
// backend always adds TransferDst to device-local buffers so they can be uploaded into.
enum class BufferUsage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
};

// What a texture can be used for. ColorAttachment = rendered into; Sampled = read in a shader;
// TransferSrc/Dst = copied from/to (the readback in the M3 proof needs TransferSrc).
enum class TextureUsage : std::uint32_t {
    None = 0,
    ColorAttachment = 1u << 0,
    DepthStencil = 1u << 1,
    Sampled = 1u << 2,
    TransferSrc = 1u << 3,
    TransferDst = 1u << 4,
};

// Where a resource's memory lives, expressed by *access pattern* rather than heap type — VMA picks
// the actual heap. GpuOnly: device-local, fastest for the GPU, not host-visible. CpuToGpu:
// host-visible, write-combined; for data the CPU writes once and the GPU reads (vertex/uniform
// uploads). GpuToCpu: host-visible, cached; for results the CPU reads back (the proof's readback).
enum class MemoryUsage : std::uint8_t {
    GpuOnly,
    CpuToGpu,
    GpuToCpu,
};

// What happens to an attachment's contents at the start (LoadOp) and end (StoreOp) of a render.
// Clear = overwrite with the clear value; Load = keep what's there; DontCare = contents undefined
// (a real performance win on tiled GPUs when you're going to overwrite everything anyway).
enum class LoadOp : std::uint8_t { Load, Clear, DontCare };
enum class StoreOp : std::uint8_t { Store, DontCare };

enum class ShaderStage : std::uint8_t { Vertex, Fragment, Compute };

enum class PrimitiveTopology : std::uint8_t { TriangleList, TriangleStrip, LineList, PointList };

enum class CullMode : std::uint8_t { None, Front, Back };

// ── Adapter (physical GPU) description ──────────────────────────────────────────────────────
enum class DeviceType : std::uint8_t { Other, IntegratedGpu, DiscreteGpu, VirtualGpu, Cpu };

// Human-facing facts about the GPU the device was created on. Logged at startup so a bug report or
// CI log tells us exactly what rendered (e.g. "llvmpipe (LLVM 17)" on a CI runner, a real GPU on a
// dev box, or "Apple M-series" via MoltenVK).
struct AdapterInfo {
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    DeviceType type = DeviceType::Other;
    std::uint32_t api_version = 0; // packed Vulkan version; format with rhi version helpers/logging
};

// ── Bit-flag operators ──────────────────────────────────────────────────────────────────────
// `enum class` is type-safe but drops the bitwise operators we want for usage flags. This macro
// re-adds just the operators a flag set needs, scoped to one enum, so `BufferUsage::Vertex |
// BufferUsage::TransferSrc` works and `has(usage, BufferUsage::Vertex)` tests membership — with no
// implicit conversions leaking elsewhere.
#define RIME_RHI_FLAGS(E)                                                                           \
    constexpr E operator|(E a, E b) noexcept {                                                      \
        return static_cast<E>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));       \
    }                                                                                              \
    constexpr E operator&(E a, E b) noexcept {                                                      \
        return static_cast<E>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));       \
    }                                                                                              \
    constexpr E& operator|=(E& a, E b) noexcept {                                                   \
        a = a | b;                                                                                 \
        return a;                                                                                  \
    }                                                                                              \
    [[nodiscard]] constexpr bool has(E set, E flag) noexcept {                                      \
        return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;           \
    }

RIME_RHI_FLAGS(BufferUsage)
RIME_RHI_FLAGS(TextureUsage)

} // namespace rime::rhi
