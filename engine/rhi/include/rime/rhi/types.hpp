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
struct Sampler;

using BufferHandle = core::Handle<Buffer>;
using TextureHandle = core::Handle<Texture>;
using ShaderHandle = core::Handle<Shader>;
using PipelineHandle = core::Handle<Pipeline>;
using SamplerHandle = core::Handle<Sampler>;

// A handle to work submitted through Device::submit() — one in-flight batch of GPU commands. Unlike
// the resource handles above it carries a monotonic id (not a generational slot): poll it with
// Device::is_complete() (non-blocking) or block on Device::wait(). A default-constructed ticket is
// "nothing in flight" — is_complete() reports true, wait() is a no-op. This is the async-submission
// seam ADR-0030 (s1.1) adds so the frame tap can hide the synchronous glass-to-CPU readback stall.
struct SubmitTicket {
    std::uint64_t id = 0; // 0 = invalid / nothing submitted

    [[nodiscard]] bool is_valid() const noexcept { return id != 0; }
};

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
    RG32Float,   // vec2 vertex attribute (e.g. UVs)
    RGB32Float,  // vec3 vertex attribute (positions, colors)
    RGBA32Float, // vec4 vertex attribute
    RGBA16Float, // the HDR scene-color target (M5.1b): float16 keeps >1 radiance for the tonemap
                 // pass at half the bytes of RGBA32F; color+blend support is spec-mandatory
    D32Float,    // depth (arrives when we add a depth pre-pass in M5)
    D32FloatS8,  // combined depth + 8-bit stencil (the cross-section cap, ADR-0014)
    R32Uint,     // one 32-bit UNSIGNED INTEGER per pixel — an ID buffer, not a color. The editor's
                 // pick pass (m9.6) rasterizes entity ids into it so "what is under this pixel?"
                 // is answered by the depth test instead of CPU ray casting. Color-attachment
                 // support for R32_UINT is spec-mandatory; integer targets never blend.
    // One 16-bit SIGNED-NORMALIZED channel: the GPU decodes the stored int16 as
    // clamp(int16 / 32767, -1, 1) on sample/imageLoad and encodes the inverse on imageStore. This
    // is the SDF clipmap's narrow-band storage (m10.4b, ADR-0032 §10) — a signed distance there is
    // always bounded (clamped to a per-level band before storing), so a normalized 2-byte channel
    // loses nothing a raw f32 would have kept *for that use*, at a quarter the bytes. Using it as
    // a STORAGE image (imageStore/imageLoad, not just sampling) needs the
    // `shaderStorageImageExtendedFormats` device feature — see the Vulkan backend's device setup.
    R16Snorm,
    R8Snorm, // the 8-bit sibling of R16Snorm (int8 / 127). Reserved for a coarser/cheaper
             // clipmap level or a lower-precision instance field if profiling ever asks for
             // one; m10.4b ships every level as R16Snorm and does not use this yet.

    // ── Block-compressed formats (m16.1, ADR-0039) ────────────────────────────────────────────
    //
    // Fixed-rate GPU-native compression: the hardware samples these directly, so they cost a
    // quarter (BC7) or half (BC5) of RGBA8 in memory AND in bandwidth, permanently — unlike a PNG,
    // which is only small on disk. This is the one lever that makes a town's worth of 2048² texture
    // sets fit.
    //
    // Sizes are per 4x4 BLOCK, not per texel, which is why the cooked-texture reader's
    // `size == width * height * 4` rule has to become a per-format block computation, and why an
    // extent that is not a multiple of 4 rounds up to whole blocks.
    //
    // Availability is NOT guaranteed — see `AdapterInfo::block_compression`.
    BC7Unorm, // 16 bytes/block. High-quality RGBA. The default for base colour and ORM.
    BC7Srgb,  // the sRGB view of the same bits, for colour rather than data
    BC5Unorm, // 16 bytes/block, TWO channels (RG). The normal-map format: store X and Y, and
              // reconstruct Z = sqrt(1 - x² - y²) in the shader, since a tangent-space normal is
              // unit-length by construction. NOTE the current shader reads z from the texture
              // (`pbr_forward_shadowed.frag`), so adopting BC5 for normals requires that change —
              // which is why BC7 is the zero-shader-churn option for normals in the meantime.
};

// How a format is laid out in memory: the size of one addressable block and its byte cost.
//
// Uncompressed formats are 1x1 blocks, so `bytes_per_block` is simply bytes per texel and the size
// maths below degenerates to the familiar `width * height * bpp`. Block-compressed formats are 4x4,
// which is why they need this at all: a 5-texel-wide BC7 image occupies TWO blocks per row, not
// 1.25, and a reader that computes `width * height * bytes` for one would reject a correct file.
struct FormatBlockInfo {
    std::uint32_t block_width = 1;
    std::uint32_t block_height = 1;
    std::uint32_t bytes_per_block = 0; // 0 ⇒ this format has no defined image size (see below)
};

[[nodiscard]] constexpr FormatBlockInfo format_block_info(Format f) noexcept {
    switch (f) {
        case Format::R8Unorm:
        case Format::R8Snorm:
            return {1, 1, 1};
        case Format::R16Snorm:
            return {1, 1, 2};
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::BGRA8Unorm:
        case Format::BGRA8Srgb:
        case Format::R32Uint:
        case Format::D32Float:
            return {1, 1, 4};
        case Format::D32FloatS8:
            return {1, 1, 5}; // packed depth+stencil; sized for completeness, never image-copied
        case Format::RG32Float:
        case Format::RGBA16Float:
            return {1, 1, 8};
        case Format::RGB32Float:
            return {1, 1, 12};
        case Format::RGBA32Float:
            return {1, 1, 16};
        case Format::BC7Unorm:
        case Format::BC7Srgb:
        case Format::BC5Unorm:
            return {4, 4, 16}; // 16 bytes per 4x4 block ⇒ 8 bpp for BC7, and the same for BC5
        case Format::Undefined:
            break;
    }
    // Vertex-attribute-only and undefined formats have no image size. Returning 0 rather than
    // guessing is deliberate: a caller sizing an allocation gets an obviously wrong answer it can
    // check, instead of a plausible one it cannot.
    return {1, 1, 0};
}

// Bytes one mip level of `format` at `width` x `height` occupies, tightly packed.
//
// Block formats round UP to whole blocks, which is the rule that makes a 1x1 mip of a BC7 chain
// still cost a full 16-byte block — the single most common way a block-compressed size computation
// goes wrong. Returns 0 for a format with no defined image size.
[[nodiscard]] constexpr std::uint64_t
format_image_size(Format format, std::uint32_t width, std::uint32_t height) noexcept {
    const FormatBlockInfo info = format_block_info(format);
    if (info.bytes_per_block == 0) {
        return 0;
    }
    const std::uint64_t bw =
        (static_cast<std::uint64_t>(width) + info.block_width - 1) / info.block_width;
    const std::uint64_t bh =
        (static_cast<std::uint64_t>(height) + info.block_height - 1) / info.block_height;
    return bw * bh * info.bytes_per_block;
}

// Is this a block-compressed format? Consumers gate on `AdapterInfo::block_compression` before
// creating a texture with one.
[[nodiscard]] constexpr bool is_block_compressed(Format f) noexcept {
    return format_block_info(f).block_width > 1;
}

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
    Storage = 1u << 5, // written/read by shaders via imageStore/imageLoad (compute, M5.2)
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

// Which pipeline stages can see a resource binding. A flag set (RIME_RHI_FLAGS below) distinct
// from ShaderStage: ShaderStage names what one shader module *is*, StageMask names the set of
// stages a binding is *visible to* — one binding often serves several stages (the mesh renderer's
// field volume is sampled in both the vertex and fragment shader).
enum class StageMask : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Fragment = 1u << 1,
    Compute = 1u << 2,
};

// What kind of resource a shader binding names — the RHI's small, intentional subset of Vulkan's
// descriptor types (ADR-0020). UniformBuffer = small read-only constants (GLSL `uniform` block);
// CombinedImageSampler = a texture + how to sample it, in one binding (GLSL `sampler2D/3D`);
// StorageBuffer / StorageImage = read-write shader access — declared now so the enum is complete,
// wired up when compute lands (M5.2).
enum class BindingType : std::uint8_t {
    UniformBuffer,
    CombinedImageSampler,
    StorageBuffer,
    StorageImage,
};

// How a fragment's color combines with what the color target already holds. A tiny preset list
// rather than raw source/destination blend factors — presets cover what real passes need and stay
// teachable; the full factor matrix can arrive if a technique ever demands it. None = overwrite
// (opaque geometry). Alpha = classic "over" transparency: out = src.a·src + (1−src.a)·dst (the UI
// overlay, transparents). Additive = out = src + dst, saturating (light accumulation, glows).
// One mode applies to all of a pipeline's color attachments (per-attachment blends when needed).
enum class BlendMode : std::uint8_t { None, Alpha, Additive };

// The most color attachments one raster pass may write (MRT). 8 matches every desktop
// implementation we target (lavapipe, MoltenVK, the majors); the Vulkan-guaranteed floor is 4,
// so a pass that insists on >4 targets narrows its device support — flagged here, checked
// nowhere yet (measure/need first).
inline constexpr std::uint32_t kMaxColorAttachments = 8;

// Timestamp slots per command buffer (M5.3): enough for begin/end pairs around every pass of a
// far bigger frame than M5 draws, while keeping the backend's query pool fixed-size.
inline constexpr std::uint32_t kMaxTimestamps = 64;

// What a texture is being used AS at a point in the frame — the RHI's abstract spelling of
// Vulkan's image layout + stage/access pairs (M5.4, ADR-0019). The render graph derives one
// state per (pass, resource) from declared accesses and emits explicit transitions between
// them; the backend maps each state to the precise synchronization2 masks. Deliberately coarse:
// one state answers "who writes/reads it and how", not per-stage micro-scoping (that precision
// arrives when profiles ask for it).
enum class ResourceState : std::uint8_t {
    Undefined,        // contents don't matter (fresh transient; first use)
    ColorTarget,      // written as a color attachment
    DepthTarget,      // written/tested as the depth(-stencil) attachment
    ShaderRead,       // sampled or uniform-read by any shader stage
    StorageReadWrite, // imageLoad/imageStore or storage-buffer access (general layout)
    TransferSrc,      // blit/copy source
    TransferDst,      // blit/copy destination
    Present,          // handed to the swapchain for presentation
};

enum class PrimitiveTopology : std::uint8_t { TriangleList, TriangleStrip, LineList, PointList };

enum class CullMode : std::uint8_t { None, Front, Back };

// How a fragment's depth is compared against the value already in the depth buffer (the depth
// test). With our 0=near .. 1=far depth convention, `Less` is the usual choice — a fragment is kept
// only if it is nearer than what was drawn before, which is what makes opaque 3-D draw correctly
// regardless of submission order. Mirrors VkCompareOp one-to-one; reused for the stencil test when
// that lands.
enum class CompareOp : std::uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

// Index buffer element width. 16-bit indexes halve bandwidth and suffice for meshes up to 65k
// vertices (the common case); 32-bit covers the rest.
enum class IndexType : std::uint8_t { Uint16, Uint32 };

// How a texture is sampled. Filter is the min/mag interpolation (Nearest = blocky, exact texels;
// Linear = smooth); AddressMode is what happens outside [0,1] UVs. A small, intentional subset.
enum class Filter : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Repeat, ClampToEdge };

// What happens to the stencil value on a (stencil) test result. The cross-section cap (ADR-0014)
// needs Keep, Replace, and the wrapping increment/decrement (to count surfaces along a view ray).
// Mirrors VkStencilOp; a small subset, grown as needed.
enum class StencilOp : std::uint8_t {
    Keep,
    Zero,
    Replace,
    IncrementWrap,
    DecrementWrap,
    Invert,
};

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
    // The DRIVER behind the device, as the backend reports it: `driver_name` is the vendor's short
    // name ("NVIDIA", "radv"), `driver_info` its own version string ("610.43.03", "Mesa 26.1.6").
    //
    // Text rather than a decoded version number on purpose. Every vendor packs the raw integer
    // differently (NVIDIA does not use Vulkan's own major/minor/patch layout), so any "pretty"
    // decode is wrong for someone. The one consumer that needs this — the perf report's machine
    // fingerprint (core/diagnostics/perf_report.hpp) — only ever asks whether two strings are
    // EQUAL, because a driver update must invalidate a committed baseline rather than silently
    // compare across it. Empty when the backend cannot report it.
    std::string driver_name;
    std::string driver_info;
    // A "portability" implementation (VK_KHR_portability_subset — MoltenVK/Metal, D3D translation
    // layers) rather than a native Vulkan driver. Such drivers only guarantee a subset of Vulkan
    // and have translation-specific sharp edges; callers that hit one (e.g. GPU tests) can gate on
    // this.
    bool portability = false;

    // ── Capabilities (m16.1) ──────────────────────────────────────────────────────────────────
    //
    // The first thing on this struct that is not a human-facing fact but a decision input. It lives
    // here rather than behind a new `Device::capabilities()` virtual because `adapter()` is already
    // the "what am I running on" query and `portability` already sets the precedent of a flag
    // callers gate behind.
    //
    // `block_compression` is Vulkan's `textureCompressionBC`: the BC1-BC7 family, which the asset
    // pipeline emits for base colour, ORM and normals. It is NOT universal — the Vulkan spec
    // requires a device to support BC *or* ETC2 *or* ASTC, not all three — so a mobile-class or
    // software driver may legitimately say no.
    //
    // The rule for consumers, ratified in ADR-0039: a device without this must FAIL a
    // block-compressed load with a named counter and a warn-once. It must never silently substitute
    // the placeholder, because a fallback path nothing exercises is a fallback that does not work.
    bool block_compression = false;
};

// ── Bit-flag operators ──────────────────────────────────────────────────────────────────────
// `enum class` is type-safe but drops the bitwise operators we want for usage flags. This macro
// re-adds just the operators a flag set needs, scoped to one enum, so `BufferUsage::Vertex |
// BufferUsage::TransferSrc` works and `has(usage, BufferUsage::Vertex)` tests membership — with no
// implicit conversions leaking elsewhere.
#define RIME_RHI_FLAGS(E)                                                                          \
    constexpr E operator|(E a, E b) noexcept {                                                     \
        return static_cast<E>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));      \
    }                                                                                              \
    constexpr E operator&(E a, E b) noexcept {                                                     \
        return static_cast<E>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));      \
    }                                                                                              \
    constexpr E& operator|=(E& a, E b) noexcept {                                                  \
        a = a | b;                                                                                 \
        return a;                                                                                  \
    }                                                                                              \
    [[nodiscard]] constexpr bool has(E set, E flag) noexcept {                                     \
        return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;          \
    }

RIME_RHI_FLAGS(BufferUsage)
RIME_RHI_FLAGS(TextureUsage)
RIME_RHI_FLAGS(StageMask)

} // namespace rime::rhi
