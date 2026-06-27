// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/rhi/rhi.hpp"

#include "mesh_render.hpp"

// Vector-field warp (C3): displace the part's surface by a computed vec3 field — a structural
// displacement or a modal mode shape — and colour it by the field magnitude. The displacement is read
// in the vertex shader (vertex texture fetch of the field volume) and scaled by an animated gain, so a
// modal mode "breathes". Reuses the mesh's vertex buffer and field volume; the push block is MeshPush
// (field_scale.w = warp gain, field_bias.w = vmag_max). See docs/math/colormap.md.
namespace rime::viewer {

struct Warp {
    rhi::ShaderHandle vsh;
    rhi::ShaderHandle fsh;
    rhi::PipelineHandle pipeline;
};

[[nodiscard]] inline Warp make_warp(rhi::Device& device,
                                    rhi::Format color_format,
                                    rhi::Format depth_format,
                                    const std::uint32_t* vert_spirv,
                                    std::size_t vert_bytes,
                                    const std::uint32_t* frag_spirv,
                                    std::size_t frag_bytes) {
    using namespace rime::rhi;
    Warp w;

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "warp.vert";
    w.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "warp.frag";
    w.fsh = device.create_shader(fsd);

    const VertexAttribute attrs[] = {
        {0, Format::RGB32Float, offsetof(MeshVertex, px)},
        {1, Format::RGB32Float, offsetof(MeshVertex, nx)},
    };
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = w.vsh;
    pd.fragment_shader = w.fsh;
    pd.vertex_layout.stride = sizeof(MeshVertex);
    pd.vertex_layout.attributes = attrs;
    pd.color_format = color_format;
    pd.cull = CullMode::None;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = CompareOp::Less;
    pd.depth_format = depth_format;
    pd.sampled_texture = true; // the vec3 field volume, sampled in the vertex (and unused in fragment)
    pd.push_constant_size = sizeof(MeshPush);
    pd.debug_name = "icem-warp-pipeline";
    w.pipeline = device.create_graphics_pipeline(pd);
    return w;
}

inline void destroy_warp(rhi::Device& device, const Warp& w) {
    device.destroy(w.pipeline);
    device.destroy(w.fsh);
    device.destroy(w.vsh);
}

// Record one warped frame: clear, then draw the mesh through the warp pipeline (push.field_scale.w is
// the current animated gain). gpu.field_tex must hold the vec3 field volume (xyz = vector, w = valid).
inline void record_warp(rhi::CommandBuffer& cmd,
                        const GpuMesh& gpu,
                        const Warp& warp,
                        rhi::TextureHandle color,
                        rhi::TextureHandle depth,
                        rhi::Extent2D extent,
                        const MeshPush& push,
                        rhi::ClearColor clear) {
    using namespace rime::rhi;

    RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = LoadOp::Clear;
    ri.color.store_op = StoreOp::Store;
    ri.color.clear = clear;
    DepthStencilAttachment da{};
    da.target = depth;
    da.load_op = LoadOp::Clear;
    da.clear_depth = 1.0f;
    ri.depth_stencil = da;

    cmd.begin_rendering(ri);
    cmd.bind_pipeline(warp.pipeline);
    cmd.bind_texture(0, gpu.field_tex, gpu.field_sampler);
    cmd.push_constants(&push, sizeof(MeshPush));
    cmd.bind_vertex_buffer(gpu.vbuf, 0);
    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);
    Rect2D sc{};
    sc.width = extent.width;
    sc.height = extent.height;
    cmd.set_scissor(sc);
    cmd.draw(gpu.vertex_count);
    cmd.end_rendering();
}

// Off-screen warped render (snapshot mode + the warp test): builds its own colour + depth targets, the
// mesh (with the vec3 field volume) and the warp pipeline, records one frame, returns the RGBA8 pixels.
[[nodiscard]] inline std::vector<std::uint8_t> render_warp_offscreen(rhi::Device& device,
                                                                     std::uint32_t size,
                                                                     const CpuMesh& cpu,
                                                                     const MeshPush& push,
                                                                     rhi::ClearColor clear,
                                                                     const std::uint32_t* mesh_vert_spirv,
                                                                     std::size_t mesh_vert_bytes,
                                                                     const std::uint32_t* mesh_frag_spirv,
                                                                     std::size_t mesh_frag_bytes,
                                                                     const std::uint32_t* warp_vert_spirv,
                                                                     std::size_t warp_vert_bytes,
                                                                     const std::uint32_t* warp_frag_spirv,
                                                                     std::size_t warp_frag_bytes,
                                                                     const float* field_rgba,
                                                                     std::uint32_t fnx,
                                                                     std::uint32_t fny,
                                                                     std::uint32_t fnz) {
    using namespace rime::rhi;
    // The mesh owns the vbuf + the (vec3) field volume; its own pipeline/shaders go unused here.
    const GpuMesh mesh = make_mesh(device, Format::RGBA8Unorm, Format::D32Float, cpu, mesh_vert_spirv,
                                   mesh_vert_bytes, mesh_frag_spirv, mesh_frag_bytes, field_rgba, fnx, fny, fnz);
    const Warp warp = make_warp(device, Format::RGBA8Unorm, Format::D32Float, warp_vert_spirv,
                                warp_vert_bytes, warp_frag_spirv, warp_frag_bytes);

    TextureDesc ctd{};
    ctd.extent = {size, size};
    ctd.format = Format::RGBA8Unorm;
    ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    const TextureHandle color = device.create_texture(ctd);

    TextureDesc dtd{};
    dtd.extent = {size, size};
    dtd.format = Format::D32Float;
    dtd.usage = TextureUsage::DepthStencil;
    const TextureHandle depth = device.create_texture(dtd);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    const BufferHandle readback = device.create_buffer(rbd);

    auto cmd = device.begin_commands();
    record_warp(*cmd, mesh, warp, color, depth, {size, size}, push, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(depth);
    device.destroy(color);
    destroy_warp(device, warp);
    destroy_mesh(device, mesh);
    return pixels;
}

} // namespace rime::viewer
