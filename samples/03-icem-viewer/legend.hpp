// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>

#include "rime/rhi/rhi.hpp"

// The field-colormap legend: a small vertical colour-scale bar drawn over the rendered frame as a
// second dynamic-rendering pass that LOADs (preserves) the mesh and paints the bar on top. No vertex
// buffer, no depth — a 4-vertex triangle strip generated in legend.vert. Header-only on rime::rhi,
// matching the viewer's other helpers (make_/record_/destroy_).
namespace rime::viewer {

struct Legend {
    rhi::ShaderHandle vsh;
    rhi::ShaderHandle fsh;
    rhi::PipelineHandle pipeline;
};

[[nodiscard]] inline Legend make_legend(rhi::Device& device,
                                        rhi::Format color_format,
                                        const std::uint32_t* vert_spirv,
                                        std::size_t vert_bytes,
                                        const std::uint32_t* frag_spirv,
                                        std::size_t frag_bytes) {
    using namespace rime::rhi;
    Legend lg;

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "legend.vert";
    lg.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "legend.frag";
    lg.fsh = device.create_shader(fsd);

    GraphicsPipelineDesc pd{};
    pd.vertex_shader = lg.vsh;
    pd.fragment_shader = lg.fsh;
    pd.color_format = color_format;
    pd.topology = PrimitiveTopology::TriangleStrip; // 4 corners → the bar quad
    pd.cull = CullMode::None;
    pd.debug_name = "icem-legend-pipeline";
    lg.pipeline = device.create_graphics_pipeline(pd);
    return lg;
}

inline void destroy_legend(rhi::Device& device, const Legend& lg) {
    device.destroy(lg.pipeline);
    device.destroy(lg.fsh);
    device.destroy(lg.vsh);
}

// Draw the bar over an already-rendered frame: a LoadOp::Load color pass (no depth) on the same target.
inline void record_legend(rhi::CommandBuffer& cmd,
                          const Legend& lg,
                          rhi::TextureHandle color,
                          rhi::Extent2D extent) {
    using namespace rime::rhi;
    RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = LoadOp::Load; // keep the mesh already drawn this frame
    ri.color.store_op = StoreOp::Store;
    cmd.begin_rendering(ri);
    cmd.bind_pipeline(lg.pipeline);

    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);
    Rect2D sc{};
    sc.width = extent.width;
    sc.height = extent.height;
    cmd.set_scissor(sc);

    cmd.draw(4); // triangle strip
    cmd.end_rendering();
}

} // namespace rime::viewer
