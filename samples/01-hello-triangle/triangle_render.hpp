// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/rhi/rhi.hpp"

// The shared body of Rime's "first pixels" render — one solid triangle, drawn through the agnostic
// rime::rhi API (no Vulkan here). Three callers share it so the exact render they exercise is
// identical:
//   - tests/rhi/offscreen_triangle_test : asserts on the pixels (the CI gate, M3.3)
//   - samples/01-hello-triangle (off-screen): ASCII-previews the pixels
//   - samples/01-hello-triangle (windowed, M3.4): presents the triangle in a real window
// The split is: make_triangle() builds the GPU resources for a given color-target format;
// record_triangle() records one clear+draw into a command buffer; render_triangle_offscreen() wires
// them to an off-screen image + readback (unchanged behavior). The caller passes the compiled SPIR-V
// so this header needn't know how the shaders were embedded.
namespace rime_sample {

// The triangle's GPU resources, built once for a given color-target format (the pipeline bakes the
// format in, so windowed rendering must build against the swapchain's format). The caller owns these
// and releases them with destroy_triangle().
struct TriangleResources {
    rime::rhi::BufferHandle vbuf;
    rime::rhi::ShaderHandle vsh;
    rime::rhi::ShaderHandle fsh;
    rime::rhi::PipelineHandle pipeline;
};

[[nodiscard]] inline TriangleResources make_triangle(rime::rhi::Device& device,
                                                     rime::rhi::Format color_format,
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
    static const Vertex vertices[] = {
        {0.0f, -0.5f, 1.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
        {-0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
    };

    TriangleResources t;

    BufferDesc vbd{};
    vbd.size = sizeof(vertices);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu; // host-visible, so initial_data uploads directly (no staging)
    vbd.initial_data = vertices;
    vbd.debug_name = "triangle-vertices";
    t.vbuf = device.create_buffer(vbd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "triangle.vert";
    t.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "triangle.frag";
    t.fsh = device.create_shader(fsd);

    static const VertexAttribute attributes[] = {
        {0, Format::RG32Float, 0},                  // position at byte offset 0
        {1, Format::RGB32Float, sizeof(float) * 2}, // color at byte offset 8
    };
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = t.vsh;
    pd.fragment_shader = t.fsh;
    pd.vertex_layout.stride = sizeof(Vertex);
    pd.vertex_layout.attributes = attributes;
    pd.color_format = color_format; // off-screen: RGBA8Unorm; windowed: the swapchain's format
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None; // draw regardless of winding — keeps the proof robust
    pd.debug_name = "triangle-pipeline";
    t.pipeline = device.create_graphics_pipeline(pd);

    return t;
}

inline void destroy_triangle(rime::rhi::Device& device, const TriangleResources& t) {
    device.destroy(t.pipeline);
    device.destroy(t.fsh);
    device.destroy(t.vsh);
    device.destroy(t.vbuf);
}

// Record clear -> draw the triangle into `target` (whose render area is `extent`), between
// begin/end rendering. Viewport/scissor are dynamic so the same pipeline draws to any target size.
inline void record_triangle(rime::rhi::CommandBuffer& cmd,
                            const TriangleResources& t,
                            rime::rhi::TextureHandle target,
                            rime::rhi::Extent2D extent,
                            rime::rhi::ClearColor clear) {
    using namespace rime::rhi;

    RenderingInfo ri{};
    ri.color.target = target;
    ri.color.load_op = LoadOp::Clear;
    ri.color.store_op = StoreOp::Store;
    ri.color.clear = clear;
    cmd.begin_rendering(ri);
    cmd.bind_pipeline(t.pipeline);
    cmd.bind_vertex_buffer(t.vbuf, 0);

    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);

    Rect2D scissor{};
    scissor.width = extent.width;
    scissor.height = extent.height;
    cmd.set_scissor(scissor);

    cmd.draw(3);
    cmd.end_rendering();
}

// The off-screen render used by the M3.3 proof (behavior unchanged): build the triangle at
// RGBA8Unorm, render it into an off-screen image, copy it into a host-visible buffer, and return the
// tightly-packed RGBA8 pixels for the caller to assert on or preview.
[[nodiscard]] inline std::vector<std::uint8_t>
render_triangle_offscreen(rime::rhi::Device& device,
                          std::uint32_t size,
                          rime::rhi::ClearColor clear,
                          const std::uint32_t* vert_spirv,
                          std::size_t vert_bytes,
                          const std::uint32_t* frag_spirv,
                          std::size_t frag_bytes) {
    using namespace rime::rhi;

    const TriangleResources tri =
        make_triangle(device, Format::RGBA8Unorm, vert_spirv, vert_bytes, frag_spirv, frag_bytes);

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

    // Record: clear -> draw the triangle -> copy the image into the readback buffer.
    auto cmd = device.begin_commands();
    record_triangle(*cmd, tri, color, {size, size}, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    // Release the resources we made (the device would also free them at teardown, but explicit is
    // clearer and keeps repeated calls from accumulating).
    device.destroy(readback);
    device.destroy(color);
    destroy_triangle(device, tri);

    return pixels;
}

} // namespace rime_sample
