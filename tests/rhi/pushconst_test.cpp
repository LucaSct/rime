// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the push-constant brick (B1a). One pipeline draws a full-screen triangle whose color comes
// entirely from a push constant; we render it into two targets with two different push-constant colors
// in the same command buffer, then read back and assert each target got its color. That proves (a) a
// push constant reaches the shader and (b) it can change per draw — the per-draw fast path the lit mesh
// renderer relies on for its MVP matrix. Off-screen + readback, so it runs GPU-free on lavapipe in CI.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"

#include "pushconst.frag.spv.h"
#include "pushconst.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi push constants reach the shader and change per draw") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping push-constant render");
        return;
    }

    const std::uint32_t size = 32;

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = pushconst_vert_spv;
    vsd.spirv_size_bytes = sizeof(pushconst_vert_spv);
    vsd.debug_name = "pushconst.vert";
    const ShaderHandle vsh = device->create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = pushconst_frag_spv;
    fsd.spirv_size_bytes = sizeof(pushconst_frag_spv);
    fsd.debug_name = "pushconst.frag";
    const ShaderHandle fsh = device->create_shader(fsd);

    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vsh;
    pd.fragment_shader = fsh;
    pd.color_format = Format::RGBA8Unorm;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None;
    pd.push_constant_size = sizeof(float) * 4; // a single vec4 color
    pd.debug_name = "pushconst-pipeline";
    const PipelineHandle pipe = device->create_graphics_pipeline(pd);

    const auto make_color = [&](const char* name) {
        TextureDesc td{};
        td.extent = {size, size};
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        td.debug_name = name;
        return device->create_texture(td);
    };
    const TextureHandle color_a = make_color("pc-color-a");
    const TextureHandle color_b = make_color("pc-color-b");

    const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
    const auto make_readback = [&](const char* name) {
        BufferDesc rbd{};
        rbd.size = bytes;
        rbd.usage = BufferUsage::TransferDst;
        rbd.memory = MemoryUsage::GpuToCpu;
        rbd.debug_name = name;
        return device->create_buffer(rbd);
    };
    const BufferHandle rb_a = make_readback("pc-readback-a");
    const BufferHandle rb_b = make_readback("pc-readback-b");

    const auto record_pass =
        [&](CommandBuffer& cmd, TextureHandle color, std::array<float, 4> rgba) {
            RenderingInfo ri{};
            ri.color.target = color;
            ri.color.load_op = LoadOp::Clear;
            ri.color.store_op = StoreOp::Store;
            ri.color.clear = {0.0f, 0.0f, 0.0f, 1.0f};
            cmd.begin_rendering(ri);
            cmd.bind_pipeline(pipe);
            cmd.push_constants(rgba.data(), static_cast<std::uint32_t>(rgba.size() * sizeof(float)));

            Viewport vp{};
            vp.width = static_cast<float>(size);
            vp.height = static_cast<float>(size);
            vp.max_depth = 1.0f;
            cmd.set_viewport(vp);
            Rect2D sc{};
            sc.width = size;
            sc.height = size;
            cmd.set_scissor(sc);

            cmd.draw(3);
            cmd.end_rendering();
        };

    auto cmd = device->begin_commands();
    record_pass(*cmd, color_a, {1.0f, 0.0f, 0.0f, 1.0f}); // red
    record_pass(*cmd, color_b, {0.0f, 1.0f, 0.0f, 1.0f}); // green
    cmd->copy_texture_to_buffer(color_a, rb_a);
    cmd->copy_texture_to_buffer(color_b, rb_b);
    device->submit_blocking(*cmd);

    std::vector<std::uint8_t> px_a(bytes), px_b(bytes);
    device->read_buffer(rb_a, px_a.data(), px_a.size(), 0);
    device->read_buffer(rb_b, px_b.data(), px_b.size(), 0);

    const std::size_t center = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
    CHECK(px_a[center + 0] > 200); // A is red
    CHECK(px_a[center + 1] < 60);
    CHECK(px_b[center + 1] > 200); // B is green
    CHECK(px_b[center + 0] < 60);

    device->destroy(rb_b);
    device->destroy(rb_a);
    device->destroy(color_b);
    device->destroy(color_a);
    device->destroy(pipe);
    device->destroy(fsh);
    device->destroy(vsh);
}
