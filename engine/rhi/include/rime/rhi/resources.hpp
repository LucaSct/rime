// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "rime/rhi/types.hpp"

// Plain-data *descriptors*: the inputs to Device::create_* . Each is a small struct of POD fields
// with sensible defaults, so call sites read like a recipe ("a 256x256 RGBA8 color target") and
// future fields can be added without breaking existing callers. Descriptors are consumed
// synchronously during creation; the backend copies out what it needs and never retains a pointer
// past the call (so spans/pointers may reference caller-owned temporaries).
namespace rime::rhi {

struct BufferDesc {
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryUsage memory = MemoryUsage::GpuOnly;

    // Optional one-shot upload at creation. Only valid for host-visible memory (CpuToGpu/GpuToCpu)
    // in M3 — a device-local buffer with initial data needs a staging copy, which lands with the
    // transfer path in a later brick. If set, exactly `size` bytes are read from here.
    const void* initial_data = nullptr;

    std::string_view debug_name = {}; // surfaced to validation layers / GPU debuggers
};

struct TextureDesc {
    Extent2D extent = {1, 1};
    // Number of depth slices. 1 (the default) is an ordinary 2-D image; >1 makes a 3-D (volume)
    // texture of extent width×height×depth, sampled in a shader with a `sampler3D` at normalized
    // (u,v,w). Volumes are how a scalar/vector simulation field is uploaded for trilinear sampling
    // (the ICEM viewer's field colormap / slice). See ADR-0013.
    std::uint32_t depth = 1;
    // Mip levels (M5.3). 1 (the default) = just the base image, exactly the pre-M5.3 behavior.
    // >1 allocates a chain of progressively halved levels; write_texture() fills level 0 from the
    // caller's pixels and generates the rest by GPU downsampling blits. Mipmaps are how sampling
    // stays alias-free under minification: the hardware picks the level whose texels are closest
    // to one-per-pixel (pair with SamplerDesc::mip_filter). Clamped to the chain length the
    // extent supports; 2-D only for now (a mip-mapped volume has no consumer yet).
    std::uint32_t mip_levels = 1;
    // Array layers (m10.1a, ADR-0032 §10). 1 (the default) is a single-layer image. >1 makes a
    // *layered* 2-D image — an array of same-sized slices sampled with `sampler2DArray`, and
    // rendered into one layer at a time (ColorAttachment::layer / DepthStencilAttachment::layer).
    // This is how cascaded shadow maps store their N cascades, and how any layered-render technique
    // (cube-face shadows, probe capture, VSM) stores its slices. Distinct from `depth`: an array is
    // N independent 2-D images with no inter-layer filtering, a 3-D volume is ONE image trilinearly
    // sampled across w — the two are mutually exclusive (array_layers > 1 ⇒ depth == 1).
    std::uint32_t array_layers = 1;
    // Cube map (m10.1a): view the layers as the 6 faces of a cube (+X,−X,+Y,−Y,+Z,−Z), sampled with
    // `samplerCube` by direction — how a point light stores its omnidirectional shadow. Requires
    // `array_layers` to be a positive multiple of 6 (6 for one cube, 6·N for a cube array). Each
    // face is still rendered into as its own layer.
    bool cube = false;
    Format format = Format::RGBA8Unorm;
    TextureUsage usage = TextureUsage::None;
    std::string_view debug_name = {};
};

// One mip level's pixels for Device::write_texture_mips: tightly packed, in the texture's format,
// covering that level's (halved) extent. The asset pipeline generates the whole chain offline and
// gamma-correctly (ADR-0024), so the RHI uploads each level verbatim instead of GPU-downsampling
// from level 0 the way write_texture does.
struct MipData {
    std::span<const std::byte> pixels;
};

// A sampler describes *how* a shader reads a texture — the min/mag filtering and what happens for
// UVs outside [0,1]. It is decoupled from the texture (the same image can be read different ways),
// and is created once via Device::create_sampler then bound alongside a texture
// (CommandBuffer::bind_texture).
struct SamplerDesc {
    Filter mag_filter = Filter::Linear;
    Filter min_filter = Filter::Linear;
    // How to read a mip-mapped texture ACROSS its levels (M5.3): Nearest snaps to the closest
    // level (visible "LOD pop" bands, but exact — and the right choice for textures with no
    // chain); Linear blends the two straddling levels — with linear min/mag that is trilinear
    // filtering, the standard for minified surfaces like a ground plane.
    Filter mip_filter = Filter::Nearest;
    // Anisotropic filtering (M5.3): 0 (or 1) = off; N up to 16 lets the sampler take N samples
    // along the axis a surface is squashed in screen space — what keeps a floor readable at
    // grazing angles where plain trilinear smears. Clamped to the device limit; silently off if
    // the device lacks the feature (a documented degrade, not an error).
    float max_anisotropy = 0.0f;
    AddressMode address_mode = AddressMode::Repeat;
    // Depth-compare sampling (m10.1a, ADR-0032 §10). When enabled the sampler compares each fetched
    // depth texel against a reference the shader supplies — a `sampler2DShadow`/`samplerCubeShadow`
    // read — and returns the (bilinearly-filtered) fraction that passed, i.e. hardware PCF, instead
    // of the raw depth. `compare_op` is that test: LessEqual (the default) means "lit if the
    // surface is at or nearer than the recorded occluder", the shadow-map convention under our
    // 0=near..1=far depth. Ignored by ordinary sampled reads; reuses the CompareOp enum the depth
    // test speaks.
    bool compare_enable = false;
    CompareOp compare_op = CompareOp::LessEqual;
    std::string_view debug_name = {};
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;

    // SPIR-V is a stream of 32-bit words. We point at the words (not bytes) plus the size in bytes,
    // matching how the SPIR-V is embedded by the offline compile step (ADR-0008).
    const std::uint32_t* spirv = nullptr;
    std::size_t spirv_size_bytes = 0;

    std::string_view entry_point = "main";
    std::string_view debug_name = {};
};

// One shader resource binding in descriptor set 0: which `binding` index it occupies, what kind
// of resource lives there, and which stages read it. A pipeline declares its whole binding layout
// up front (the *shape* of its descriptor set — what a shader's `layout(set=0, binding=N)`
// declarations promise); the actual resources are attached at record time with
// CommandBuffer::bind_uniform_buffer / bind_texture and baked into a transient descriptor set at
// the next draw. This is the M5.1 descriptor model v2 (ADR-0020): one set, N declared bindings —
// richer than M3.5's single cached combined image-sampler, still deliberately far from bindless.
struct BindingDesc {
    std::uint32_t binding = 0;
    BindingType type = BindingType::CombinedImageSampler;
    StageMask stages = StageMask::Vertex | StageMask::Fragment;
};

// One vertex attribute: which shader `location` it feeds, its `format`, and its byte `offset`
// within a vertex. (location/format/offset is exactly the trio a graphics API needs to interpret
// raw vertex bytes.)
struct VertexAttribute {
    std::uint32_t location = 0;
    Format format = Format::RGB32Float;
    std::uint32_t offset = 0;
};

// The layout of one interleaved vertex buffer: the stride (bytes per vertex) and its attributes.
// M3 uses a single vertex buffer; multiple bindings/instancing are a later addition.
struct VertexLayout {
    std::uint32_t stride = 0;
    std::span<const VertexAttribute> attributes = {};
};

// One side's stencil behaviour: how the stencil value changes on test/depth failure or pass, and
// the comparison against the reference (masked by `stencil_read_mask`). Defaults are inert (always
// pass, keep the value), so a pipeline with stencil off ignores these. See ADR-0014.
struct StencilFace {
    StencilOp fail = StencilOp::Keep;       // stencil test failed
    StencilOp depth_fail = StencilOp::Keep; // stencil passed but depth failed
    StencilOp pass = StencilOp::Keep;       // both passed
    CompareOp compare = CompareOp::Always;  // (ref & read_mask) <compare> (value & read_mask)
};

// Everything needed to bake a graphics pipeline. In M3 a pipeline targets a single color
// attachment via dynamic rendering (no VkRenderPass object — see ADR-0007), so the desc carries the
// color format directly rather than a render-pass handle.
struct GraphicsPipelineDesc {
    ShaderHandle vertex_shader;
    // Optional since M5.6: leave it default (invalid) for a DEPTH-ONLY pipeline — rasterization
    // without a fragment stage is valid Vulkan and exactly what a depth pre-pass wants (vertex
    // transform + fixed-function depth test, no shading). Such a pipeline must also set
    // `color_format = Format::Undefined` (no color attachments).
    ShaderHandle fragment_shader;
    VertexLayout vertex_layout = {};
    // The single color attachment's format — or Format::Undefined for a depth-only pipeline
    // (zero color attachments; pair with a color-less RenderingInfo).
    Format color_format = Format::RGBA8Unorm;
    // Multiple render targets (M5.1b): the formats of ALL color attachments this pipeline writes,
    // position-matched to RenderingInfo::colors. When non-empty it wins over `color_format`
    // (which remains the ergonomic single-target spelling); ≤ kMaxColorAttachments entries. The
    // fragment shader's layout(location = i) outputs map to attachment i.
    std::span<const Format> color_formats = {};
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cull = CullMode::None;

    // How this pipeline's fragments combine with the color target(s) — see BlendMode. None (the
    // default) overwrites, exactly the pre-M5.1b behavior. Applies to every color attachment.
    BlendMode blend = BlendMode::None;

    // Depth state. Off by default, so the existing flat-2D pipelines (triangle, quad) are
    // unchanged. When `depth_test` is on, the pass must supply a matching DepthStencilAttachment
    // and `depth_format` must equal that attachment's format (dynamic rendering matches pipeline
    // and pass by format, not by a render-pass object). `depth_write` controls whether passing
    // fragments update the depth buffer (turn it off for, e.g., a translucent overlay that should
    // test but not occlude).
    bool depth_test = false;
    bool depth_write = true;
    CompareOp depth_compare = CompareOp::Less;
    Format depth_format = Format::Undefined;

    // Stencil state (ADR-0014). Off by default. When on, the pass must supply a stencil-capable
    // depth-stencil attachment (a D32FloatS8 target) and `depth_format` names it. `stencil_front` /
    // `stencil_back` are the two-sided ops (front- vs back-facing triangles) — with cull off, one
    // draw can increment on back faces and decrement on front faces, which is how the cross-section
    // cap counts where the cut plane is inside the solid. Reference/masks are baked (static) for
    // now.
    bool stencil_test = false;
    StencilFace stencil_front = {};
    StencilFace stencil_back = {};
    std::uint32_t stencil_read_mask = 0xFF;
    std::uint32_t stencil_write_mask = 0xFF;
    std::uint32_t stencil_reference = 0;

    // When false, the pipeline writes no colour (colorWriteMask = 0): the stencil-marking pass
    // updates only the stencil buffer, leaving the rendered image untouched. Default true (normal
    // colour output).
    bool color_write = true;
    // The pipeline's set-0 binding layout (ADR-0020) — see BindingDesc above. Empty (and
    // sampled_texture false) means the pipeline binds no resources. The span is consumed during
    // creation like everything else in a descriptor: the backend copies it out.
    std::span<const BindingDesc> bindings = {};

    // Sugar predating `bindings` (M3.5): true declares exactly {binding 0, CombinedImageSampler,
    // Vertex|Fragment} — the "one texture" model every pre-M5 pipeline used. Ignored when
    // `bindings` is non-empty. Kept so existing callers compile unchanged; new code should
    // declare `bindings` explicitly.
    bool sampled_texture = false;

    // Bytes of push-constant data the pipeline accepts, visible to both the vertex and fragment
    // stages. 0 (the default) means none. Push constants are the smallest way to hand a shader a
    // little per-draw data — the model-view-projection matrix, a clip plane, a colormap range —
    // without a descriptor set or buffer. Keep it ≤128 bytes, the value guaranteed on every Vulkan
    // device. Set the data per draw with CommandBuffer::push_constants. (Uniform buffers, for
    // larger/shared data, arrive with the render graph / material layer.)
    std::uint32_t push_constant_size = 0;

    std::string_view debug_name = {};
};

// Everything needed to bake a compute pipeline (M5.2, ADR-0021) — deliberately tiny next to the
// graphics desc, because compute has no fixed-function state: one shader plus the same declared
// set-0 binding layout (ADR-0020) and push-constant budget the graphics path uses. Storage
// bindings (StorageBuffer / StorageImage) are compute's bread and butter — how a kernel reads and
// writes bulk data — but the layout model is shared, so a compute pipeline may also read UBOs and
// sampled textures.
struct ComputePipelineDesc {
    ShaderHandle shader; // a ShaderStage::Compute module
    std::span<const BindingDesc> bindings = {};
    std::uint32_t push_constant_size = 0; // visible to the compute stage
    std::string_view debug_name = {};
};

} // namespace rime::rhi
