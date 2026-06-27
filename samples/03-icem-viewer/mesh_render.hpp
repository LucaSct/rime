// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"

// GPU-side rendering for the ICEM viewer's lit mesh pass: upload a CpuMesh, build a depth-tested lit
// pipeline, and record one draw per frame with the camera's matrix pushed as a push constant. Header-
// only and built on the agnostic rime::rhi (no Vulkan here), so the windowed app, the off-screen
// snapshot mode, and the pixel-readback test all share exactly one render — the same "one source of
// truth" split the triangle/quad helpers use (make_* builds resources, record_* records a frame,
// *_offscreen wires an off-screen target + readback).
namespace rime::viewer {

// The per-draw push-constant block — must match the layout in shaders/mesh.{vert,frag}. 128 bytes,
// the portable push-constant budget (ADR-0012).
struct MeshPush {
    core::Mat4 mvp;       // clip-from-world (proj * view); 64 bytes
    float cam_pos[4];     // world-space eye position (xyz) + pad; 16 bytes
    // Cross-section half-space: a fragment is discarded where dot(plane.xyz, world_pos) > plane.w, so
    // (plane.xyz = unit normal, plane.w = signed offset) cuts the part open along that plane. The
    // disabled state is plane = (0,0,0, +big): dot is 0, never exceeds w, so nothing is clipped.
    float clip_plane[4];  // 16 bytes
    // Field colormap (C1). field_scale.xyz / field_bias.xyz are the affine world→texcoord map
    // (uvw = world * scale + bias); field_scale.w = vmin, field_bias.w = vmax are the colormap domain.
    // The field is "on" iff vmax > vmin — so the zero-initialised default (all zero) means "no field".
    float field_scale[4]; // 16 bytes
    float field_bias[4];  // 16 bytes
};
static_assert(sizeof(MeshPush) == 128,
              "MeshPush must match the shader push_constant block (mat4 + 4*vec4)");

// A clip plane that is disabled (clips nothing) — the default for a non-sectioned view.
[[nodiscard]] inline MeshPush with_no_clip(MeshPush p) noexcept {
    p.clip_plane[0] = p.clip_plane[1] = p.clip_plane[2] = 0.0f;
    p.clip_plane[3] = 1e30f;
    return p;
}

// GPU resources for one mesh, owned by the caller and released with destroy_mesh().
struct GpuMesh {
    rhi::BufferHandle vbuf;
    std::uint32_t vertex_count = 0;
    rhi::ShaderHandle vsh;
    rhi::ShaderHandle fsh;
    rhi::PipelineHandle pipeline;
    // The field volume sampled by mesh.frag's colormap. Always present (the pipeline declares a
    // sampler3D): a real field when one is loaded, else a 1×1×2 zero "dummy" the shader never samples
    // (the push constant leaves the field off). 3-D so it matches the sampler3D binding (ADR-0013).
    rhi::TextureHandle field_tex;
    rhi::SamplerHandle field_sampler;
};

[[nodiscard]] inline GpuMesh make_mesh(rhi::Device& device,
                                       rhi::Format color_format,
                                       rhi::Format depth_format,
                                       const CpuMesh& cpu,
                                       const std::uint32_t* vert_spirv,
                                       std::size_t vert_bytes,
                                       const std::uint32_t* frag_spirv,
                                       std::size_t frag_bytes,
                                       const float* field_rgba = nullptr,
                                       std::uint32_t fnx = 1,
                                       std::uint32_t fny = 1,
                                       std::uint32_t fnz = 1) {
    using namespace rime::rhi;
    GpuMesh m;
    m.vertex_count = static_cast<std::uint32_t>(cpu.vertices.size());

    // Field volume (RGBA32F, trilinear, clamp-to-edge): a real field (field_rgba = fnx·fny·fnz·4
    // floats, R=value G=validity) or a 1×1×2 zero dummy. Either way a 3-D image, so it binds to the
    // shader's sampler3D (ADR-0013); the dummy is never sampled (the push constant leaves the field off).
    static const float kDummyVol[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const bool has_field = field_rgba != nullptr;
    TextureDesc ftd{};
    ftd.extent = {has_field ? fnx : 1u, has_field ? fny : 1u};
    ftd.depth = has_field ? fnz : 2u;
    ftd.format = Format::RGBA32Float;
    ftd.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    ftd.debug_name = "icem-field-volume";
    m.field_tex = device.create_texture(ftd);
    const std::uint64_t field_bytes = static_cast<std::uint64_t>(ftd.extent.width) * ftd.extent.height *
                                      ftd.depth * 4 * sizeof(float);
    device.write_texture(m.field_tex, has_field ? field_rgba : kDummyVol, field_bytes);

    SamplerDesc fsm{};
    fsm.mag_filter = Filter::Linear; // trilinear field sampling
    fsm.min_filter = Filter::Linear;
    fsm.address_mode = AddressMode::ClampToEdge; // a field does not tile
    fsm.debug_name = "icem-field-sampler";
    m.field_sampler = device.create_sampler(fsm);

    BufferDesc vbd{};
    vbd.size = cpu.vertices.size() * sizeof(MeshVertex);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu; // host-visible: upload the soup directly, no staging
    vbd.initial_data = cpu.vertices.data();
    vbd.debug_name = "icem-mesh-vertices";
    m.vbuf = device.create_buffer(vbd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "mesh.vert";
    m.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "mesh.frag";
    m.fsh = device.create_shader(fsd);

    const VertexAttribute attrs[] = {
        {0, Format::RGB32Float, offsetof(MeshVertex, px)}, // position
        {1, Format::RGB32Float, offsetof(MeshVertex, nx)}, // normal
    };
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = m.vsh;
    pd.fragment_shader = m.fsh;
    pd.vertex_layout.stride = sizeof(MeshVertex);
    pd.vertex_layout.attributes = attrs;
    pd.color_format = color_format;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None; // soup winding isn't guaranteed; two-sided shading handles both faces
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = CompareOp::Less;
    pd.depth_format = depth_format;
    pd.push_constant_size = sizeof(MeshPush);
    pd.sampled_texture = true; // set 0 / binding 0 = the field volume (sampler3D) — see mesh.frag
    pd.debug_name = "icem-mesh-pipeline";
    m.pipeline = device.create_graphics_pipeline(pd);
    return m;
}

inline void destroy_mesh(rhi::Device& device, const GpuMesh& m) {
    device.destroy(m.pipeline);
    device.destroy(m.fsh);
    device.destroy(m.vsh);
    device.destroy(m.field_sampler);
    device.destroy(m.field_tex);
    device.destroy(m.vbuf);
}

// Record one lit frame: clear color + depth, then draw the mesh with the camera matrix pushed in.
inline void record_mesh(rhi::CommandBuffer& cmd,
                        const GpuMesh& m,
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
    cmd.bind_pipeline(m.pipeline);
    cmd.bind_texture(0, m.field_tex, m.field_sampler); // the field volume for mesh.frag's colormap
    cmd.push_constants(&push, sizeof(MeshPush));
    cmd.bind_vertex_buffer(m.vbuf, 0);

    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);
    Rect2D sc{};
    sc.width = extent.width;
    sc.height = extent.height;
    cmd.set_scissor(sc);

    cmd.draw(m.vertex_count);
    cmd.end_rendering();
}

// Off-screen render of a mesh to a square RGBA8 image, returned as tightly-packed bytes — the path the
// snapshot mode writes to a file and the render test asserts on. Builds its own color + depth targets.
[[nodiscard]] inline std::vector<std::uint8_t> render_mesh_offscreen(rhi::Device& device,
                                                                     std::uint32_t size,
                                                                     const CpuMesh& cpu,
                                                                     const MeshPush& push,
                                                                     rhi::ClearColor clear,
                                                                     const std::uint32_t* vert_spirv,
                                                                     std::size_t vert_bytes,
                                                                     const std::uint32_t* frag_spirv,
                                                                     std::size_t frag_bytes,
                                                                     const float* field_rgba = nullptr,
                                                                     std::uint32_t fnx = 1,
                                                                     std::uint32_t fny = 1,
                                                                     std::uint32_t fnz = 1) {
    using namespace rime::rhi;

    const GpuMesh mesh = make_mesh(device, Format::RGBA8Unorm, Format::D32Float, cpu, vert_spirv,
                                   vert_bytes, frag_spirv, frag_bytes, field_rgba, fnx, fny, fnz);

    TextureDesc ctd{};
    ctd.extent = {size, size};
    ctd.format = Format::RGBA8Unorm;
    ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    ctd.debug_name = "icem-offscreen-color";
    const TextureHandle color = device.create_texture(ctd);

    TextureDesc dtd{};
    dtd.extent = {size, size};
    dtd.format = Format::D32Float;
    dtd.usage = TextureUsage::DepthStencil;
    dtd.debug_name = "icem-offscreen-depth";
    const TextureHandle depth = device.create_texture(dtd);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "icem-offscreen-readback";
    const BufferHandle readback = device.create_buffer(rbd);

    auto cmd = device.begin_commands();
    record_mesh(*cmd, mesh, color, depth, {size, size}, push, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(depth);
    device.destroy(color);
    destroy_mesh(device, mesh);
    return pixels;
}

} // namespace rime::viewer
