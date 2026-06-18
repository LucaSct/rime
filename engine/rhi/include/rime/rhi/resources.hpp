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
    Format format = Format::RGBA8Unorm;
    TextureUsage usage = TextureUsage::None;
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

// Everything needed to bake a graphics pipeline. In M3 a pipeline targets a single color
// attachment via dynamic rendering (no VkRenderPass object — see ADR-0007), so the desc carries the
// color format directly rather than a render-pass handle.
struct GraphicsPipelineDesc {
    ShaderHandle vertex_shader;
    ShaderHandle fragment_shader;
    VertexLayout vertex_layout = {};
    Format color_format = Format::RGBA8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cull = CullMode::None;
    std::string_view debug_name = {};
};

} // namespace rime::rhi
