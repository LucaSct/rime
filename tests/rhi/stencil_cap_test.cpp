// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for ADR-0014 — stencil state, the mechanism the cross-section cap is built on. In one
// render pass into a colour + D32FloatS8 depth-stencil target it: (1) draws a centred triangle that
// writes stencil = 1 where it covers, with colour writes masked off; then (2) draws a full-screen
// triangle that passes the stencil test only where stencil == 1 and paints it green. Reading back,
// the centre (inside the marked triangle) is green and a corner (stencil still 0) stays the clear
// colour — which can only happen if stencil clear, two-sided stencil write (Replace), the
// colour-write mask, and the stencil compare all worked. Off-screen + readback, GPU-free on
// lavapipe in CI. (main() in device_test.)

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "stencil_mark.vert.spv.h"
#include "stencil_solid.frag.spv.h"
#include "volume.vert.spv.h" // the full-screen triangle (shared with the volume proof)

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi stencil: mark a region then fill only where stencil is set (ADR-0014)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping stencil render");
        return;
    }

    const auto shader = [&](ShaderStage stage, const std::uint32_t* spv, std::size_t bytes) {
        ShaderDesc sd{};
        sd.stage = stage;
        sd.spirv = spv;
        sd.spirv_size_bytes = bytes;
        return device->create_shader(sd);
    };
    const ShaderHandle mark_vs =
        shader(ShaderStage::Vertex, stencil_mark_vert_spv, sizeof(stencil_mark_vert_spv));
    const ShaderHandle full_vs =
        shader(ShaderStage::Vertex, volume_vert_spv, sizeof(volume_vert_spv));
    const ShaderHandle solid_fs =
        shader(ShaderStage::Fragment, stencil_solid_frag_spv, sizeof(stencil_solid_frag_spv));

    // Marking pipeline: write stencil = 1 where drawn, no colour, no depth. Two-sided (cull off) so
    // the ops apply regardless of winding.
    GraphicsPipelineDesc mk{};
    mk.vertex_shader = mark_vs;
    mk.fragment_shader = solid_fs;
    mk.color_format = Format::RGBA8Unorm;
    mk.cull = CullMode::None;
    mk.depth_format = Format::D32FloatS8;
    mk.stencil_test = true;
    mk.stencil_front = {StencilOp::Keep, StencilOp::Keep, StencilOp::Replace, CompareOp::Always};
    mk.stencil_back = mk.stencil_front;
    mk.stencil_reference = 1;
    mk.color_write = false;
    const PipelineHandle mark_pipe = device->create_graphics_pipeline(mk);

    // Fill pipeline: full-screen, draw green only where stencil == 1.
    GraphicsPipelineDesc fl{};
    fl.vertex_shader = full_vs;
    fl.fragment_shader = solid_fs;
    fl.color_format = Format::RGBA8Unorm;
    fl.cull = CullMode::None;
    fl.depth_format = Format::D32FloatS8;
    fl.stencil_test = true;
    fl.stencil_front = {StencilOp::Keep, StencilOp::Keep, StencilOp::Keep, CompareOp::Equal};
    fl.stencil_back = fl.stencil_front;
    fl.stencil_reference = 1;
    const PipelineHandle fill_pipe = device->create_graphics_pipeline(fl);

    const std::uint32_t size = 32;
    TextureDesc ctd{};
    ctd.extent = {size, size};
    ctd.format = Format::RGBA8Unorm;
    ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    const TextureHandle color = device->create_texture(ctd);

    TextureDesc dtd{};
    dtd.extent = {size, size};
    dtd.format = Format::D32FloatS8;
    dtd.usage = TextureUsage::DepthStencil;
    const TextureHandle ds = device->create_texture(dtd);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    const BufferHandle readback = device->create_buffer(rbd);

    auto cmd = device->begin_commands();
    {
        RenderingInfo ri{};
        ri.color.target = color;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = {0.0f, 0.0f, 0.0f, 1.0f};
        DepthStencilAttachment da{};
        da.target = ds;
        da.load_op = LoadOp::Clear; // clears depth AND stencil (to 0)
        da.clear_depth = 1.0f;
        da.clear_stencil = 0;
        ri.depth_stencil = da;

        cmd->begin_rendering(ri);
        Viewport vp{};
        vp.width = static_cast<float>(size);
        vp.height = static_cast<float>(size);
        vp.max_depth = 1.0f;
        Rect2D sc{};
        sc.width = size;
        sc.height = size;

        cmd->bind_pipeline(mark_pipe); // stencil := 1 on the centred triangle
        cmd->set_viewport(vp);
        cmd->set_scissor(sc);
        cmd->draw(3);

        cmd->bind_pipeline(fill_pipe); // green where stencil == 1
        cmd->set_viewport(vp);
        cmd->set_scissor(sc);
        cmd->draw(3);
        cmd->end_rendering();
    }
    cmd->copy_texture_to_buffer(color, readback);
    device->submit_blocking(*cmd);

    std::vector<std::uint8_t> px(byte_count);
    device->read_buffer(readback, px.data(), px.size(), 0);

    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return &px[(static_cast<std::size_t>(y) * size + x) * 4];
    };
    const std::uint8_t* center = at(size / 2, size / 2);
    const std::uint8_t* corner = at(0, 0);
    CHECK(center[1] > 128); // centre filled green (stencil was set there)
    CHECK(center[0] < 128);
    CHECK(corner[0] < 40); // corner never marked → stays the clear colour
    CHECK(corner[1] < 40);

    device->destroy(readback);
    device->destroy(ds);
    device->destroy(color);
    device->destroy(fill_pipe);
    device->destroy(mark_pipe);
    device->destroy(solid_fs);
    device->destroy(full_vs);
    device->destroy(mark_vs);
}
