// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/rhi/rhi.hpp"

#include "mesh_render.hpp"

// GPU raymarched isosurface / DVR of a scalar field (C2): a full-screen pass that marches the field
// volume per pixel (iso.frag). No geometry, no depth — it reuses the same RGBA32F field volume the
// colormap built (held by a GpuMesh) and draws the isosurface (an isotherm) or a cloudy DVR. See
// docs/math/raymarch.md.
namespace rime::viewer {

// Push block for the raymarch — must match shaders/iso.frag. 112 bytes.
struct IsoPush {
    core::Mat4 inv_vp;     // 64  world-from-clip (inverse view-projection)
    float field_scale[4]; // 16  xyz world->uvw scale, w = isovalue
    float field_bias[4];  // 16  xyz world->uvw bias,  w = vmin
    float meta[4];         // 16  x = vmax, y = step count, z = mode (0 isosurface, 1 DVR)
};
static_assert(sizeof(IsoPush) == 112, "IsoPush must match shaders/iso.frag");

struct Iso {
    rhi::ShaderHandle vsh;
    rhi::ShaderHandle fsh;
    rhi::PipelineHandle pipeline;
};

[[nodiscard]] inline Iso make_iso(rhi::Device& device,
                                  rhi::Format color_format,
                                  const std::uint32_t* vert_spirv,
                                  std::size_t vert_bytes,
                                  const std::uint32_t* frag_spirv,
                                  std::size_t frag_bytes) {
    using namespace rime::rhi;
    Iso it;

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "iso.vert";
    it.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "iso.frag";
    it.fsh = device.create_shader(fsd);

    GraphicsPipelineDesc pd{};
    pd.vertex_shader = it.vsh;
    pd.fragment_shader = it.fsh;
    pd.color_format = color_format;
    pd.cull = CullMode::None;
    pd.sampled_texture = true; // the field volume
    pd.push_constant_size = sizeof(IsoPush);
    pd.debug_name = "icem-iso-pipeline";
    it.pipeline = device.create_graphics_pipeline(pd);
    return it;
}

inline void destroy_iso(rhi::Device& device, const Iso& it) {
    device.destroy(it.pipeline);
    device.destroy(it.fsh);
    device.destroy(it.vsh);
}

// Record the full-screen raymarch into `color` (color-only, no depth — the isosurface discards where it
// isn't hit). `field_tex`/`field_sampler` are the scalar field volume (e.g. a GpuMesh's).
inline void record_iso(rhi::CommandBuffer& cmd,
                       const Iso& it,
                       rhi::TextureHandle field_tex,
                       rhi::SamplerHandle field_sampler,
                       rhi::TextureHandle color,
                       rhi::Extent2D extent,
                       const IsoPush& push,
                       rhi::ClearColor clear) {
    using namespace rime::rhi;
    RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = LoadOp::Clear;
    ri.color.store_op = StoreOp::Store;
    ri.color.clear = clear;
    cmd.begin_rendering(ri);
    cmd.bind_pipeline(it.pipeline);
    cmd.bind_texture(0, field_tex, field_sampler);
    cmd.push_constants(&push, sizeof(IsoPush));
    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);
    Rect2D sc{};
    sc.width = extent.width;
    sc.height = extent.height;
    cmd.set_scissor(sc);
    cmd.draw(3); // full-screen triangle
    cmd.end_rendering();
}

// Off-screen isosurface/DVR render (snapshot + test). Builds a GpuMesh only to own the field volume
// (its vbuf/pipeline go unused), the iso pipeline, a colour target + readback; records one frame.
[[nodiscard]] inline std::vector<std::uint8_t> render_iso_offscreen(rhi::Device& device,
                                                                    std::uint32_t size,
                                                                    const CpuMesh& cpu,
                                                                    const IsoPush& push,
                                                                    rhi::ClearColor clear,
                                                                    const std::uint32_t* mesh_vert_spirv,
                                                                    std::size_t mesh_vert_bytes,
                                                                    const std::uint32_t* mesh_frag_spirv,
                                                                    std::size_t mesh_frag_bytes,
                                                                    const std::uint32_t* iso_vert_spirv,
                                                                    std::size_t iso_vert_bytes,
                                                                    const std::uint32_t* iso_frag_spirv,
                                                                    std::size_t iso_frag_bytes,
                                                                    const float* field_rgba,
                                                                    std::uint32_t fnx,
                                                                    std::uint32_t fny,
                                                                    std::uint32_t fnz) {
    using namespace rime::rhi;
    const GpuMesh mesh = make_mesh(device, Format::RGBA8Unorm, Format::D32Float, cpu, mesh_vert_spirv,
                                   mesh_vert_bytes, mesh_frag_spirv, mesh_frag_bytes, field_rgba, fnx, fny, fnz);
    const Iso it = make_iso(device, Format::RGBA8Unorm, iso_vert_spirv, iso_vert_bytes, iso_frag_spirv,
                            iso_frag_bytes);

    TextureDesc ctd{};
    ctd.extent = {size, size};
    ctd.format = Format::RGBA8Unorm;
    ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    const TextureHandle color = device.create_texture(ctd);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    const BufferHandle readback = device.create_buffer(rbd);

    auto cmd = device.begin_commands();
    record_iso(*cmd, it, mesh.field_tex, mesh.field_sampler, color, {size, size}, push, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(color);
    destroy_iso(device, it);
    destroy_mesh(device, mesh);
    return pixels;
}

} // namespace rime::viewer
