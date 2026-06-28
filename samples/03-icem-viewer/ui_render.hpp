// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// GPU side of the from-scratch UI (E2): the font-atlas texture, the UI pipeline, and a host-visible
// vertex buffer that the immediate-mode `Ui` batch is streamed into each frame (write_buffer). The
// UI is drawn as one pass *over* the rendered 3-D scene (color load_op = Load, depth off),
// alpha-tested so the panel is opaque and the text crisp. Header-only on rime::rhi; see ui.hpp /
// ui_font.hpp and docs/math/ui-text-layout.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "ui.hpp"
#include "ui_font.hpp"

namespace rime::viewer::ui {

// Push block for ui.vert: the framebuffer size (so screen-pixel positions map to NDC). 16 bytes.
struct UiPush {
    float screen[2];
    float pad[2];
};

struct UiRenderer {
    rhi::ShaderHandle vsh;
    rhi::ShaderHandle fsh;
    rhi::PipelineHandle pipeline;
    rhi::TextureHandle atlas;
    rhi::SamplerHandle sampler;
    rhi::BufferHandle vbuf;
    std::uint32_t capacity = 0;     // vertices the buffer can hold
    std::uint32_t vertex_count = 0; // vertices uploaded for the current frame
};

// Build the atlas, sampler, pipeline and a vertex buffer sized for `max_vertices` (a fixed,
// generous capacity — the UI overlay is tiny, so we never grow it mid-flight).
[[nodiscard]] inline UiRenderer make_ui_renderer(rhi::Device& device,
                                                 rhi::Format color_format,
                                                 const std::uint32_t* vert_spirv,
                                                 std::size_t vert_bytes,
                                                 const std::uint32_t* frag_spirv,
                                                 std::size_t frag_bytes,
                                                 std::uint32_t max_vertices = 60000) {
    using namespace rime::rhi;
    UiRenderer r;

    // Font atlas (RGBA8, coverage in every channel; the shader reads .r). Linear-filtered so the 2×
    // upscaled bitmap edges resolve cleanly under the 0.5 alpha test.
    const std::vector<std::uint8_t> pixels = build_font_atlas();
    TextureDesc td{};
    td.extent = {kAtlasW, kAtlasH};
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    td.debug_name = "icem-ui-atlas";
    r.atlas = device.create_texture(td);
    device.write_texture(r.atlas, pixels.data(), pixels.size());

    SamplerDesc sm{};
    sm.mag_filter = Filter::Linear;
    sm.min_filter = Filter::Linear;
    sm.address_mode = AddressMode::ClampToEdge;
    sm.debug_name = "icem-ui-sampler";
    r.sampler = device.create_sampler(sm);

    BufferDesc vbd{};
    vbd.size = static_cast<std::uint64_t>(max_vertices) * sizeof(UiVertex);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu; // host-visible: streamed each frame with write_buffer
    vbd.debug_name = "icem-ui-vertices";
    r.vbuf = device.create_buffer(vbd);
    r.capacity = max_vertices;

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "ui.vert";
    r.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "ui.frag";
    r.fsh = device.create_shader(fsd);

    const VertexAttribute attrs[] = {
        {0, Format::RG32Float, offsetof(UiVertex, x)},   // screen position
        {1, Format::RG32Float, offsetof(UiVertex, u)},   // atlas texcoord
        {2, Format::RGBA32Float, offsetof(UiVertex, r)}, // colour
    };
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = r.vsh;
    pd.fragment_shader = r.fsh;
    pd.vertex_layout.stride = sizeof(UiVertex);
    pd.vertex_layout.attributes = attrs;
    pd.color_format = color_format;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None;
    pd.depth_test = false; // an overlay: always on top, no depth
    pd.sampled_texture = true;
    pd.push_constant_size = sizeof(UiPush);
    pd.debug_name = "icem-ui-pipeline";
    r.pipeline = device.create_graphics_pipeline(pd);
    return r;
}

inline void destroy_ui_renderer(rhi::Device& device, const UiRenderer& r) {
    device.destroy(r.pipeline);
    device.destroy(r.fsh);
    device.destroy(r.vsh);
    device.destroy(r.vbuf);
    device.destroy(r.sampler);
    device.destroy(r.atlas);
}

// Stream this frame's UI batch into the vertex buffer (clamped to capacity). Call before record_ui.
inline void upload_ui(rhi::Device& device, UiRenderer& r, const Ui& ui) {
    const std::vector<UiVertex>& v = ui.vertices();
    r.vertex_count = static_cast<std::uint32_t>(v.size() < r.capacity ? v.size() : r.capacity);
    if (r.vertex_count > 0)
        device.write_buffer(
            r.vbuf, v.data(), static_cast<std::size_t>(r.vertex_count) * sizeof(UiVertex));
}

// Draw the uploaded UI batch over `color` (load_op = Load preserves the 3-D scene already there).
inline void record_ui(rhi::CommandBuffer& cmd,
                      const UiRenderer& r,
                      rhi::TextureHandle color,
                      rhi::Extent2D extent,
                      rhi::LoadOp load_op = rhi::LoadOp::Load,
                      rhi::ClearColor clear = {}) {
    using namespace rime::rhi;
    if (r.vertex_count == 0)
        return;
    RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = load_op;
    ri.color.store_op = StoreOp::Store;
    ri.color.clear = clear;
    cmd.begin_rendering(ri);
    cmd.bind_pipeline(r.pipeline);
    cmd.bind_texture(0, r.atlas, r.sampler);
    UiPush push{};
    push.screen[0] = static_cast<float>(extent.width);
    push.screen[1] = static_cast<float>(extent.height);
    cmd.push_constants(&push, sizeof(UiPush));
    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);
    Rect2D sc{};
    sc.width = extent.width;
    sc.height = extent.height;
    cmd.set_scissor(sc);
    cmd.bind_vertex_buffer(r.vbuf, 0);
    cmd.draw(r.vertex_count);
    cmd.end_rendering();
}

// Off-screen UI render (snapshot + test): clear a square target and draw the UI batch onto it.
[[nodiscard]] inline std::vector<std::uint8_t> render_ui_offscreen(rhi::Device& device,
                                                                   std::uint32_t size,
                                                                   const Ui& ui,
                                                                   rhi::ClearColor clear,
                                                                   const std::uint32_t* vert_spirv,
                                                                   std::size_t vert_bytes,
                                                                   const std::uint32_t* frag_spirv,
                                                                   std::size_t frag_bytes) {
    using namespace rime::rhi;
    UiRenderer r = make_ui_renderer(
        device, Format::RGBA8Unorm, vert_spirv, vert_bytes, frag_spirv, frag_bytes);
    upload_ui(device, r, ui);

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
    record_ui(*cmd, r, color, {size, size}, LoadOp::Clear, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(color);
    destroy_ui_renderer(device, r);
    return pixels;
}

} // namespace rime::viewer::ui
