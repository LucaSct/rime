// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/rhi/rhi.hpp"

#include "mesh_render.hpp"

// The cross-section *solid cap* (B2b): make the cut look like sawn metal — and paint the computed field
// on the cut face — by filling the cutting plane exactly where it passes through the solid. Done with
// the stencil buffer (ADR-0014) in one render pass, after the part's colour pass:
//   1. marking pass — render the part (clip-discarding the cut-away half) flipping a stencil parity bit
//      per fragment, no colour/depth. Odd parity ⇒ the plane is inside the solid at that pixel.
//   2. cap pass — draw a plane-aligned quad spanning the part; the stencil test keeps only the odd
//      (inside-solid) pixels, and cap.frag colours them (field slice, or metal).
// Parity is winding-independent (STL soup winding isn't guaranteed). See docs/math/clip-cap.md.
namespace rime::viewer {

// Push block for the cap quad — must match shaders/cap.{vert,frag}. 128 bytes.
struct CapPush {
    core::Mat4 mvp;        // 64
    float field_scale[4];  // 16  xyz world->uvw scale, w vmin   (mirrors MeshPush)
    float field_bias[4];   // 16  xyz world->uvw bias,  w vmax
    float cap_rect[4];     // 16  u_lo, u_hi, v_lo, v_hi (the two in-plane axes' extents)
    float cap_meta[4];     // 16  x = plane offset, y = axis (0/1/2)
};
static_assert(sizeof(CapPush) == 128, "CapPush must match shaders/cap.{vert,frag}");

struct Cap {
    rhi::ShaderHandle mark_vsh; // mesh.vert (reused for the marking pass)
    rhi::ShaderHandle mark_fsh; // capmark.frag
    rhi::ShaderHandle cap_vsh;  // cap.vert
    rhi::ShaderHandle cap_fsh;  // cap.frag
    rhi::PipelineHandle mark_pipeline;
    rhi::PipelineHandle cap_pipeline;
};

[[nodiscard]] inline Cap make_cap(rhi::Device& device,
                                  rhi::Format color_format,
                                  rhi::Format depth_format, // must carry stencil (D32FloatS8)
                                  const std::uint32_t* mesh_vert_spirv,
                                  std::size_t mesh_vert_bytes,
                                  const std::uint32_t* capmark_frag_spirv,
                                  std::size_t capmark_frag_bytes,
                                  const std::uint32_t* cap_vert_spirv,
                                  std::size_t cap_vert_bytes,
                                  const std::uint32_t* cap_frag_spirv,
                                  std::size_t cap_frag_bytes) {
    using namespace rime::rhi;
    Cap c;

    const auto shader = [&](ShaderStage stage, const std::uint32_t* spv, std::size_t bytes,
                            const char* name) {
        ShaderDesc sd{};
        sd.stage = stage;
        sd.spirv = spv;
        sd.spirv_size_bytes = bytes;
        sd.debug_name = name;
        return device.create_shader(sd);
    };
    c.mark_vsh = shader(ShaderStage::Vertex, mesh_vert_spirv, mesh_vert_bytes, "mesh.vert (cap mark)");
    c.mark_fsh = shader(ShaderStage::Fragment, capmark_frag_spirv, capmark_frag_bytes, "capmark.frag");
    c.cap_vsh = shader(ShaderStage::Vertex, cap_vert_spirv, cap_vert_bytes, "cap.vert");
    c.cap_fsh = shader(ShaderStage::Fragment, cap_frag_spirv, cap_frag_bytes, "cap.frag");

    // Marking pipeline: same vertex layout as the mesh (position + normal); flips stencil bit 0 on every
    // kept fragment (Invert, write mask 0x1), no colour, no depth. Cull off so both faces count.
    const VertexAttribute attrs[] = {
        {0, Format::RGB32Float, offsetof(MeshVertex, px)},
        {1, Format::RGB32Float, offsetof(MeshVertex, nx)},
    };
    GraphicsPipelineDesc mk{};
    mk.vertex_shader = c.mark_vsh;
    mk.fragment_shader = c.mark_fsh;
    mk.vertex_layout.stride = sizeof(MeshVertex);
    mk.vertex_layout.attributes = attrs;
    mk.color_format = color_format;
    mk.cull = CullMode::None;
    mk.depth_test = false; // count all surfaces along the ray, regardless of order
    mk.depth_format = depth_format;
    mk.stencil_test = true;
    mk.stencil_front = {StencilOp::Keep, StencilOp::Keep, StencilOp::Invert, CompareOp::Always};
    mk.stencil_back = mk.stencil_front;
    mk.stencil_write_mask = 0x1; // parity bit
    mk.color_write = false;
    mk.push_constant_size = sizeof(MeshPush); // reuses the mesh push (for the clip plane)
    mk.debug_name = "icem-cap-mark";
    c.mark_pipeline = device.create_graphics_pipeline(mk);

    // Cap pipeline: the plane quad, drawn where stencil parity is odd, colouring the cut face.
    GraphicsPipelineDesc cp{};
    cp.vertex_shader = c.cap_vsh;
    cp.fragment_shader = c.cap_fsh;
    cp.color_format = color_format;
    cp.cull = CullMode::None;
    cp.depth_test = false; // drawn last, gated by stencil; the cut plane is the frontmost kept surface
    cp.depth_format = depth_format;
    cp.stencil_test = true;
    cp.stencil_front = {StencilOp::Keep, StencilOp::Keep, StencilOp::Keep, CompareOp::Equal};
    cp.stencil_back = cp.stencil_front;
    cp.stencil_read_mask = 0x1;
    cp.stencil_reference = 1; // draw where parity bit == 1 (inside the solid)
    cp.sampled_texture = true; // the field volume, for the slice colour
    cp.push_constant_size = sizeof(CapPush);
    cp.debug_name = "icem-cap-fill";
    c.cap_pipeline = device.create_graphics_pipeline(cp);
    return c;
}

inline void destroy_cap(rhi::Device& device, const Cap& c) {
    device.destroy(c.cap_pipeline);
    device.destroy(c.mark_pipeline);
    device.destroy(c.cap_fsh);
    device.destroy(c.cap_vsh);
    device.destroy(c.mark_fsh);
    device.destroy(c.mark_vsh);
}

namespace detail {
inline void set_full(rhi::CommandBuffer& cmd, rhi::Extent2D extent) {
    rhi::Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);
    rhi::Rect2D sc{};
    sc.width = extent.width;
    sc.height = extent.height;
    cmd.set_scissor(sc);
}
} // namespace detail

// Record a whole sectioned frame into one render pass: clear colour + depth + stencil, draw the lit
// part, then (if do_cap) mark the stencil and fill the cap. The depth target must be a D32FloatS8.
inline void record_section(rhi::CommandBuffer& cmd,
                           const GpuMesh& gpu,
                           const Cap& cap,
                           rhi::TextureHandle color,
                           rhi::TextureHandle depth,
                           rhi::Extent2D extent,
                           const MeshPush& mesh_push,
                           const CapPush& cap_push,
                           rhi::ClearColor clear,
                           bool do_cap) {
    using namespace rime::rhi;

    RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = LoadOp::Clear;
    ri.color.store_op = StoreOp::Store;
    ri.color.clear = clear;
    DepthStencilAttachment da{};
    da.target = depth;
    da.load_op = LoadOp::Clear; // clears depth (1.0) and stencil (0)
    da.clear_depth = 1.0f;
    da.clear_stencil = 0;
    ri.depth_stencil = da;

    cmd.begin_rendering(ri);
    draw_mesh(cmd, gpu, extent, mesh_push); // lit part, clip-discarding the cut-away half + field colormap

    if (do_cap) {
        // Stencil-mark pass: flip parity on kept fragments (the mark pipeline masks colour + depth).
        cmd.bind_pipeline(cap.mark_pipeline);
        cmd.push_constants(&mesh_push, sizeof(MeshPush));
        cmd.bind_vertex_buffer(gpu.vbuf, 0);
        detail::set_full(cmd, extent);
        cmd.draw(gpu.vertex_count);

        // Cap fill: the plane quad, kept where parity is odd, coloured by the field (or metal).
        cmd.bind_pipeline(cap.cap_pipeline);
        cmd.bind_texture(0, gpu.field_tex, gpu.field_sampler);
        cmd.push_constants(&cap_push, sizeof(CapPush));
        detail::set_full(cmd, extent);
        cmd.draw(6); // two triangles
    }
    cmd.end_rendering();
}

// Off-screen sectioned render (for the snapshot mode and the cap test): builds its own colour +
// D32FloatS8 targets, the mesh (with optional field volume) and the cap, records one section, and
// returns the RGBA8 pixels. Mirrors render_mesh_offscreen but with stencil + the cap.
[[nodiscard]] inline std::vector<std::uint8_t> render_section_offscreen(
    rhi::Device& device,
    std::uint32_t size,
    const CpuMesh& cpu,
    const MeshPush& mesh_push,
    const CapPush& cap_push,
    rhi::ClearColor clear,
    bool do_cap,
    const std::uint32_t* mesh_vert_spirv,
    std::size_t mesh_vert_bytes,
    const std::uint32_t* mesh_frag_spirv,
    std::size_t mesh_frag_bytes,
    const std::uint32_t* capmark_frag_spirv,
    std::size_t capmark_frag_bytes,
    const std::uint32_t* cap_vert_spirv,
    std::size_t cap_vert_bytes,
    const std::uint32_t* cap_frag_spirv,
    std::size_t cap_frag_bytes,
    const float* field_rgba = nullptr,
    std::uint32_t fnx = 1,
    std::uint32_t fny = 1,
    std::uint32_t fnz = 1) {
    using namespace rime::rhi;

    const GpuMesh mesh = make_mesh(device, Format::RGBA8Unorm, Format::D32FloatS8, cpu, mesh_vert_spirv,
                                   mesh_vert_bytes, mesh_frag_spirv, mesh_frag_bytes, field_rgba, fnx, fny, fnz);
    const Cap cap = make_cap(device, Format::RGBA8Unorm, Format::D32FloatS8, mesh_vert_spirv,
                             mesh_vert_bytes, capmark_frag_spirv, capmark_frag_bytes, cap_vert_spirv,
                             cap_vert_bytes, cap_frag_spirv, cap_frag_bytes);

    TextureDesc ctd{};
    ctd.extent = {size, size};
    ctd.format = Format::RGBA8Unorm;
    ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    ctd.debug_name = "icem-section-color";
    const TextureHandle color = device.create_texture(ctd);

    TextureDesc dtd{};
    dtd.extent = {size, size};
    dtd.format = Format::D32FloatS8;
    dtd.usage = TextureUsage::DepthStencil;
    dtd.debug_name = "icem-section-depthstencil";
    const TextureHandle depth = device.create_texture(dtd);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "icem-section-readback";
    const BufferHandle readback = device.create_buffer(rbd);

    auto cmd = device.begin_commands();
    record_section(*cmd, mesh, cap, color, depth, {size, size}, mesh_push, cap_push, clear, do_cap);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(depth);
    device.destroy(color);
    destroy_cap(device, cap);
    destroy_mesh(device, mesh);
    return pixels;
}

} // namespace rime::viewer
