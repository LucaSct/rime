// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the descriptor-model-v2 brick (M5.1a, ADR-0020). Two claims:
//
//  (1) A uniform buffer reaches the shader through a *declared* binding layout, and re-binding a
//      different slice of the same buffer between draws changes what the next draw sees — which
//      is exactly what the transient-set model exists for (one baked descriptor set per draw, so
//      per-draw data can live as offset slices of one buffer).
//
//  (2) Descriptor capacity is no longer a cap. M3.5's device-global pool held 16 sets, ever; here
//      one command buffer bakes 24 sets (24 draws, each with a different UBO slice) and every
//      draw still lands its own value. Pools now grow with demand and recycle on completion.
//
// Off-screen + readback, so it runs GPU-free on lavapipe in CI (the M3.3 proof pattern).

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "ubo.frag.spv.h"
#include "ubo.vert.spv.h"

namespace {

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// One 256-byte UBO slice per draw: 256 is the worst-case minUniformBufferOffsetAlignment the spec
// allows, so offsets i*256 are valid on every device without querying limits.
constexpr std::uint64_t kSlice = 256;

} // namespace

TEST_CASE("rhi uniform buffers bind per draw through declared binding layouts (ADR-0020)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping uniform-buffer render");
        return;
    }

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = ubo_vert_spv;
    vsd.spirv_size_bytes = sizeof(ubo_vert_spv);
    vsd.debug_name = "ubo.vert";
    const ShaderHandle vsh = device->create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = ubo_frag_spv;
    fsd.spirv_size_bytes = sizeof(ubo_frag_spv);
    fsd.debug_name = "ubo.frag";
    const ShaderHandle fsh = device->create_shader(fsd);

    // The declared set-0 layout (the ADR-0020 path — no sampled_texture sugar): one uniform
    // buffer at binding 0, read by the fragment stage.
    const BindingDesc bindings[] = {{0, BindingType::UniformBuffer, StageMask::Fragment}};
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vsh;
    pd.fragment_shader = fsh;
    pd.color_format = Format::RGBA8Unorm;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None;
    pd.bindings = bindings;
    pd.debug_name = "ubo-pipeline";
    const PipelineHandle pipe = device->create_graphics_pipeline(pd);

    SUBCASE("two draws, two slices of one buffer, two distinct results") {
        const std::uint32_t size = 32;

        // One host-visible uniform buffer holding two color slices.
        BufferDesc ubd{};
        ubd.size = 2 * kSlice;
        ubd.usage = BufferUsage::Uniform;
        ubd.memory = MemoryUsage::CpuToGpu;
        ubd.debug_name = "ubo-two-slices";
        const BufferHandle ubo = device->create_buffer(ubd);
        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        device->write_buffer(ubo, red, sizeof(red), 0);
        device->write_buffer(ubo, green, sizeof(green), kSlice);

        const auto make_color = [&](const char* name) {
            TextureDesc td{};
            td.extent = {size, size};
            td.format = Format::RGBA8Unorm;
            td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
            td.debug_name = name;
            return device->create_texture(td);
        };
        const TextureHandle color_a = make_color("ubo-color-a");
        const TextureHandle color_b = make_color("ubo-color-b");

        const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
        const auto make_readback = [&](const char* name) {
            BufferDesc rbd{};
            rbd.size = bytes;
            rbd.usage = BufferUsage::TransferDst;
            rbd.memory = MemoryUsage::GpuToCpu;
            rbd.debug_name = name;
            return device->create_buffer(rbd);
        };
        const BufferHandle rb_a = make_readback("ubo-readback-a");
        const BufferHandle rb_b = make_readback("ubo-readback-b");

        const auto record_pass =
            [&](CommandBuffer& cmd, TextureHandle color, std::uint64_t slice_offset) {
                RenderingInfo ri{};
                ri.color.target = color;
                ri.color.load_op = LoadOp::Clear;
                ri.color.store_op = StoreOp::Store;
                ri.color.clear = {0.0f, 0.0f, 0.0f, 1.0f};
                cmd.begin_rendering(ri);
                cmd.bind_pipeline(pipe);
                cmd.bind_uniform_buffer(0, ubo, slice_offset, sizeof(float) * 4);

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
        record_pass(*cmd, color_a, 0);      // slice 0: red
        record_pass(*cmd, color_b, kSlice); // slice 1: green — same buffer, new offset, new set
        cmd->copy_texture_to_buffer(color_a, rb_a);
        cmd->copy_texture_to_buffer(color_b, rb_b);
        device->submit_blocking(*cmd);

        std::vector<std::uint8_t> px_a(bytes), px_b(bytes);
        device->read_buffer(rb_a, px_a.data(), px_a.size(), 0);
        device->read_buffer(rb_b, px_b.data(), px_b.size(), 0);

        const std::size_t center = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
        CHECK(px_a[center + 0] > 200); // A saw slice 0 (red)
        CHECK(px_a[center + 1] < 60);
        CHECK(px_b[center + 1] > 200); // B saw slice 1 (green)
        CHECK(px_b[center + 0] < 60);

        device->destroy(rb_b);
        device->destroy(rb_a);
        device->destroy(color_b);
        device->destroy(color_a);
        device->destroy(ubo);
    }

    SUBCASE("24 per-draw sets in one command buffer — past the old 16-set cap") {
        // 24 one-pixel-wide scissored columns, each drawn with its own UBO slice. Every draw
        // bakes a fresh transient set; 24 > the 16 sets the M3.5 pool could ever hand out.
        constexpr std::uint32_t kColumns = 24;
        const std::uint32_t width = kColumns, height = 8;

        BufferDesc ubd{};
        ubd.size = kColumns * kSlice;
        ubd.usage = BufferUsage::Uniform;
        ubd.memory = MemoryUsage::CpuToGpu;
        ubd.debug_name = "ubo-ramp";
        const BufferHandle ubo = device->create_buffer(ubd);
        for (std::uint32_t i = 0; i < kColumns; ++i) {
            // A red ramp, exactly representable in UNORM8: r = (10*i + 5)/255.
            const float rgba[4] = {(10.0f * i + 5.0f) / 255.0f, 0.0f, 0.0f, 1.0f};
            device->write_buffer(ubo, rgba, sizeof(rgba), i * kSlice);
        }

        TextureDesc td{};
        td.extent = {width, height};
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        td.debug_name = "ubo-ramp-target";
        const TextureHandle color = device->create_texture(td);

        const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * 4;
        BufferDesc rbd{};
        rbd.size = bytes;
        rbd.usage = BufferUsage::TransferDst;
        rbd.memory = MemoryUsage::GpuToCpu;
        rbd.debug_name = "ubo-ramp-readback";
        const BufferHandle rb = device->create_buffer(rbd);

        auto cmd = device->begin_commands();
        RenderingInfo ri{};
        ri.color.target = color;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = {0.0f, 0.0f, 1.0f, 1.0f}; // blue background: any missed column shows
        cmd->begin_rendering(ri);
        cmd->bind_pipeline(pipe);
        Viewport vp{};
        vp.width = static_cast<float>(width);
        vp.height = static_cast<float>(height);
        vp.max_depth = 1.0f;
        cmd->set_viewport(vp);
        for (std::uint32_t i = 0; i < kColumns; ++i) {
            Rect2D sc{};
            sc.x = static_cast<std::int32_t>(i);
            sc.width = 1;
            sc.height = height;
            cmd->set_scissor(sc); // clip the full-screen triangle to column i
            cmd->bind_uniform_buffer(0, ubo, i * kSlice, sizeof(float) * 4);
            cmd->draw(3);
        }
        cmd->end_rendering();
        cmd->copy_texture_to_buffer(color, rb);
        device->submit_blocking(*cmd);

        std::vector<std::uint8_t> px(bytes);
        device->read_buffer(rb, px.data(), px.size(), 0);

        for (std::uint32_t i = 0; i < kColumns; ++i) {
            const std::size_t at = (static_cast<std::size_t>(height / 2) * width + i) * 4;
            const int expect = static_cast<int>(10 * i + 5);
            CHECK(std::abs(static_cast<int>(px[at + 0]) - expect) <= 1); // its own slice's red
            CHECK(px[at + 2] < 60); // and not the blue background
        }

        device->destroy(rb);
        device->destroy(color);
        device->destroy(ubo);
    }

    device->destroy(pipe);
    device->destroy(fsh);
    device->destroy(vsh);
}
