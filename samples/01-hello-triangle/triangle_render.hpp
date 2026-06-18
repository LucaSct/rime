// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/rhi/rhi.hpp"

// The shared body of the M3.3 "first pixels" proof: render one solid triangle into an off-screen
// RGBA8 image through the RHI and hand back the pixels. Both the 01-hello-triangle sample (which
// previews them as ASCII) and tests/rhi/offscreen_triangle_test (which asserts on them) call this,
// so the exact rendering they exercise is identical. It uses only the agnostic rime::rhi API — no
// Vulkan here. The caller passes the compiled SPIR-V for the two shaders (so this header doesn't
// need to know how they were embedded).
namespace rime_sample {

[[nodiscard]] inline std::vector<std::uint8_t>
render_triangle_offscreen(rime::rhi::Device& device,
                          std::uint32_t size,
                          rime::rhi::ClearColor clear,
                          const std::uint32_t* vert_spirv,
                          std::size_t vert_bytes,
                          const std::uint32_t* frag_spirv,
                          std::size_t frag_bytes) {
    using namespace rime::rhi;

    // Interleaved vertices: vec2 position (NDC) + vec3 color. A single solid-red triangle whose
    // interior covers the image center but not its corners — that asymmetry is what the proof's
    // pixel checks rely on.
    struct Vertex {
        float x, y;    // position
        float r, g, b; // color
    };
    const Vertex vertices[] = {
        {0.0f, -0.5f, 1.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
        {-0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
    };

    BufferDesc vbd{};
    vbd.size = sizeof(vertices);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu; // host-visible, so initial_data uploads directly (no staging)
    vbd.initial_data = vertices;
    vbd.debug_name = "triangle-vertices";
    const BufferHandle vbuf = device.create_buffer(vbd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "triangle.vert";
    const ShaderHandle vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "triangle.frag";
    const ShaderHandle fsh = device.create_shader(fsd);

    const VertexAttribute attributes[] = {
        {0, Format::RG32Float, 0},                          // position at byte offset 0
        {1, Format::RGB32Float, sizeof(float) * 2},         // color at byte offset 8
    };
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vsh;
    pd.fragment_shader = fsh;
    pd.vertex_layout.stride = sizeof(Vertex);
    pd.vertex_layout.attributes = attributes;
    pd.color_format = Format::RGBA8Unorm;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None; // draw regardless of winding — keeps the proof robust
    pd.debug_name = "triangle-pipeline";
    const PipelineHandle pipeline = device.create_graphics_pipeline(pd);

    TextureDesc td{};
    td.extent = {size, size};
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc; // render to, then copy out
    td.debug_name = "triangle-target";
    const TextureHandle color = device.create_texture(td);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu; // host-readable, for the readback
    rbd.debug_name = "triangle-readback";
    const BufferHandle readback = device.create_buffer(rbd);

    // Record: clear -> draw the triangle -> end -> copy the image into the readback buffer.
    auto cmd = device.begin_commands();
    RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = LoadOp::Clear;
    ri.color.store_op = StoreOp::Store;
    ri.color.clear = clear;
    cmd->begin_rendering(ri);
    cmd->bind_pipeline(pipeline);
    cmd->bind_vertex_buffer(vbuf, 0);

    Viewport vp{};
    vp.width = static_cast<float>(size);
    vp.height = static_cast<float>(size);
    vp.max_depth = 1.0f;
    cmd->set_viewport(vp);

    Rect2D scissor{};
    scissor.width = size;
    scissor.height = size;
    cmd->set_scissor(scissor);

    cmd->draw(3);
    cmd->end_rendering();
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    // Release the resources we made (the device would also free them at teardown, but explicit is
    // clearer and keeps repeated calls from accumulating).
    device.destroy(readback);
    device.destroy(color);
    device.destroy(pipeline);
    device.destroy(fsh);
    device.destroy(vsh);
    device.destroy(vbuf);

    return pixels;
}

} // namespace rime_sample
