// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/rhi/rhi.hpp"

// The shared body of Rime's textured-quad render — M3's headline "done when", drawn through the
// agnostic rime::rhi API (no Vulkan here). It exercises everything M3.5 adds: an **index buffer**
// (a quad is 4 vertices + 6 indices), a **texture upload** (a tiny 2×2 image, staged to the GPU), a
// **sampler**, and the **descriptor model** (a combined image-sampler bound to the fragment shader).
// Two callers share it so the render is identical: tests/rhi/textured_quad_test (asserts the pixels,
// the CI gate) and samples/02-textured-quad (presents it in a window / previews it as ASCII).
namespace rime_sample {

// A 2×2 RGBA8 test texture, row-major: red, green / blue, yellow. Distinct per-texel colors so a
// nearest-sampled quad shows four solid colored quadrants — trivial to verify by eye and by assert.
[[nodiscard]] inline std::array<std::uint8_t, 16> quad_texels() {
    return {{255, 0, 0, 255, 0, 255, 0, 255,    // row 0: red,  green
             0, 0, 255, 255, 255, 255, 0, 255}}; // row 1: blue, yellow
}

struct QuadResources {
    rime::rhi::BufferHandle vbuf;
    rime::rhi::BufferHandle ibuf;
    rime::rhi::TextureHandle texture;
    rime::rhi::SamplerHandle sampler;
    rime::rhi::ShaderHandle vsh;
    rime::rhi::ShaderHandle fsh;
    rime::rhi::PipelineHandle pipeline;
};

[[nodiscard]] inline QuadResources make_quad(rime::rhi::Device& device,
                                             rime::rhi::Format color_format,
                                             const std::uint32_t* vert_spirv,
                                             std::size_t vert_bytes,
                                             const std::uint32_t* frag_spirv,
                                             std::size_t frag_bytes) {
    using namespace rime::rhi;

    // A quad covering most of the framebuffer (corners left to the clear color), interleaved
    // vec2 position (NDC) + vec2 UV; indexed so the two triangles share the four corner vertices.
    struct Vertex {
        float x, y; // NDC position
        float u, v; // texture coordinate
    };
    static const Vertex vertices[] = {
        {-0.9f, -0.9f, 0.0f, 0.0f}, // top-left      (uv 0,0)
        {0.9f, -0.9f, 1.0f, 0.0f},  // top-right     (uv 1,0)
        {0.9f, 0.9f, 1.0f, 1.0f},   // bottom-right  (uv 1,1)
        {-0.9f, 0.9f, 0.0f, 1.0f},  // bottom-left   (uv 0,1)
    };
    static const std::uint16_t indices[] = {0, 1, 2, 2, 3, 0};

    QuadResources q;

    BufferDesc vbd{};
    vbd.size = sizeof(vertices);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu;
    vbd.initial_data = vertices;
    vbd.debug_name = "quad-vertices";
    q.vbuf = device.create_buffer(vbd);

    BufferDesc ibd{};
    ibd.size = sizeof(indices);
    ibd.usage = BufferUsage::Index;
    ibd.memory = MemoryUsage::CpuToGpu;
    ibd.initial_data = indices;
    ibd.debug_name = "quad-indices";
    q.ibuf = device.create_buffer(ibd);

    // The 2×2 texture: device-local, filled via a staging copy (write_texture), then sampled.
    const std::array<std::uint8_t, 16> texels = quad_texels();
    TextureDesc td{};
    td.extent = {2, 2};
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    td.debug_name = "quad-texture";
    q.texture = device.create_texture(td);
    device.write_texture(q.texture, texels.data(), texels.size());

    SamplerDesc smd{};
    smd.mag_filter = Filter::Nearest; // blocky: each texel is one solid quadrant (clean to assert)
    smd.min_filter = Filter::Nearest;
    smd.address_mode = AddressMode::ClampToEdge;
    smd.debug_name = "quad-sampler";
    q.sampler = device.create_sampler(smd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "quad.vert";
    q.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "quad.frag";
    q.fsh = device.create_shader(fsd);

    static const VertexAttribute attrs[] = {
        {0, Format::RG32Float, 0},                 // position at byte offset 0
        {1, Format::RG32Float, sizeof(float) * 2}, // uv at byte offset 8
    };
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = q.vsh;
    pd.fragment_shader = q.fsh;
    pd.vertex_layout.stride = sizeof(Vertex);
    pd.vertex_layout.attributes = attrs;
    pd.color_format = color_format;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None;
    pd.sampled_texture = true; // declares set 0 / binding 0 = the sampler2D
    pd.debug_name = "quad-pipeline";
    q.pipeline = device.create_graphics_pipeline(pd);

    return q;
}

inline void destroy_quad(rime::rhi::Device& device, const QuadResources& q) {
    device.destroy(q.pipeline);
    device.destroy(q.fsh);
    device.destroy(q.vsh);
    device.destroy(q.sampler);
    device.destroy(q.texture);
    device.destroy(q.ibuf);
    device.destroy(q.vbuf);
}

// Record clear -> draw the textured quad (indexed) into `target`, between begin/end rendering.
inline void record_quad(rime::rhi::CommandBuffer& cmd,
                        const QuadResources& q,
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
    cmd.bind_pipeline(q.pipeline);
    cmd.bind_texture(0, q.texture, q.sampler); // after bind_pipeline: uses its descriptor layout
    cmd.bind_vertex_buffer(q.vbuf, 0);
    cmd.bind_index_buffer(q.ibuf, IndexType::Uint16, 0);

    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);

    Rect2D scissor{};
    scissor.width = extent.width;
    scissor.height = extent.height;
    cmd.set_scissor(scissor);

    cmd.draw_indexed(6); // 2 triangles, 4 shared vertices
    cmd.end_rendering();
}

// Off-screen render + readback (the form the pixel test asserts on): build the quad at RGBA8Unorm,
// render it into an off-screen image, copy it into a host-visible buffer, and return the pixels.
[[nodiscard]] inline std::vector<std::uint8_t>
render_quad_offscreen(rime::rhi::Device& device,
                      std::uint32_t size,
                      rime::rhi::ClearColor clear,
                      const std::uint32_t* vert_spirv,
                      std::size_t vert_bytes,
                      const std::uint32_t* frag_spirv,
                      std::size_t frag_bytes) {
    using namespace rime::rhi;

    const QuadResources q =
        make_quad(device, Format::RGBA8Unorm, vert_spirv, vert_bytes, frag_spirv, frag_bytes);

    TextureDesc td{};
    td.extent = {size, size};
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    td.debug_name = "quad-target";
    const TextureHandle color = device.create_texture(td);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "quad-readback";
    const BufferHandle readback = device.create_buffer(rbd);

    auto cmd = device.begin_commands();
    record_quad(*cmd, q, color, {size, size}, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(color);
    destroy_quad(device, q);
    return pixels;
}

} // namespace rime_sample
