// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the blend + MRT + HDR-format brick (M5.1b). Three claims:
//
//  (1) BlendMode combines fragments with the target: Alpha implements the "over" operator
//      (out = a·src + (1−a)·dst) and Additive saturating accumulation (out = src + dst) — checked
//      against hand-computed expectations after drawing over a known clear color.
//
//  (2) One pass writes multiple render targets: a fragment shader with two location outputs fills
//      two images in a single draw, each with its own value (RenderingInfo::colors +
//      GraphicsPipelineDesc::color_formats).
//
//  (3) RGBA16Float round-trips values ABOVE 1.0 — the property the HDR scene target exists for
//      (UNORM8 clamps at 1.0; the tonemap pass needs the >1 radiance to still be there).
//
// Off-screen + readback, GPU-free on lavapipe in CI (the M3.3 proof pattern). The blend and HDR
// cases reuse the pushconst full-screen shaders; MRT owns mrt.frag.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "mrt.frag.spv.h"
#include "pushconst.frag.spv.h"
#include "pushconst.vert.spv.h"
#include "rime/rhi/rhi.hpp"

namespace {

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// Decode one IEEE 754 binary16 (the RGBA16Float texel channel) to float. Covers what the proof
// meets — zeros, normals, and (for completeness) subnormals; no NaN/inf handling needed here.
float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 0x1;
    const std::uint32_t exp = (h >> 10) & 0x1F;
    const std::uint32_t man = h & 0x3FF;
    const float s = sign ? -1.0f : 1.0f;
    if (exp == 0)
        return s * std::ldexp(static_cast<float>(man), -24); // subnormal (or zero)
    return s * std::ldexp(static_cast<float>(man) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
}

} // namespace

TEST_CASE("rhi blending, multiple render targets, and the HDR color format (M5.1b)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping blend/MRT/HDR render");
        return;
    }

    const std::uint32_t size = 32;

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = pushconst_vert_spv;
    vsd.spirv_size_bytes = sizeof(pushconst_vert_spv);
    vsd.debug_name = "pushconst.vert (fullscreen)";
    const ShaderHandle vsh = device->create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = pushconst_frag_spv;
    fsd.spirv_size_bytes = sizeof(pushconst_frag_spv);
    fsd.debug_name = "pushconst.frag (color from push)";
    const ShaderHandle fsh = device->create_shader(fsd);

    // Draw one full-screen triangle with `rgba` pushed, over a `clear` background, through a
    // pipeline built by the caller — then read the center pixel back. Shared by the blend cases.
    const auto draw_over = [&](PipelineHandle pipe,
                               ClearColor clear,
                               std::array<float, 4> rgba,
                               Format fmt,
                               std::uint32_t texel_bytes) {
        TextureDesc td{};
        td.extent = {size, size};
        td.format = fmt;
        td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        td.debug_name = "blend-target";
        const TextureHandle color = device->create_texture(td);

        const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * texel_bytes;
        BufferDesc rbd{};
        rbd.size = bytes;
        rbd.usage = BufferUsage::TransferDst;
        rbd.memory = MemoryUsage::GpuToCpu;
        rbd.debug_name = "blend-readback";
        const BufferHandle rb = device->create_buffer(rbd);

        auto cmd = device->begin_commands();
        RenderingInfo ri{};
        ri.color.target = color;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = clear;
        cmd->begin_rendering(ri);
        cmd->bind_pipeline(pipe);
        cmd->push_constants(rgba.data(), sizeof(float) * 4);
        Viewport vp{};
        vp.width = static_cast<float>(size);
        vp.height = static_cast<float>(size);
        vp.max_depth = 1.0f;
        cmd->set_viewport(vp);
        Rect2D sc{};
        sc.width = size;
        sc.height = size;
        cmd->set_scissor(sc);
        cmd->draw(3);
        cmd->end_rendering();
        cmd->copy_texture_to_buffer(color, rb);
        device->submit_blocking(*cmd);

        std::vector<std::uint8_t> px(bytes);
        device->read_buffer(rb, px.data(), px.size(), 0);
        device->destroy(rb);
        device->destroy(color);
        return px;
    };

    const auto make_pushconst_pipeline = [&](BlendMode blend, Format fmt, const char* name) {
        GraphicsPipelineDesc pd{};
        pd.vertex_shader = vsh;
        pd.fragment_shader = fsh;
        pd.color_format = fmt;
        pd.topology = PrimitiveTopology::TriangleList;
        pd.cull = CullMode::None;
        pd.blend = blend;
        pd.push_constant_size = sizeof(float) * 4;
        pd.debug_name = name;
        return device->create_graphics_pipeline(pd);
    };

    SUBCASE("Alpha blend is the over operator") {
        const PipelineHandle pipe =
            make_pushconst_pipeline(BlendMode::Alpha, Format::RGBA8Unorm, "blend-alpha");
        // Half-transparent green over opaque red: out = 0.5·(0,1,0) + 0.5·(1,0,0) = (.5,.5,0).
        const auto px = draw_over(
            pipe, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 0.5f}, Format::RGBA8Unorm, 4);
        const std::size_t c = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
        CHECK(std::abs(static_cast<int>(px[c + 0]) - 128) <= 2);
        CHECK(std::abs(static_cast<int>(px[c + 1]) - 128) <= 2);
        CHECK(px[c + 2] == 0);
        device->destroy(pipe);
    }

    SUBCASE("Additive blend accumulates and saturates") {
        const PipelineHandle pipe =
            make_pushconst_pipeline(BlendMode::Additive, Format::RGBA8Unorm, "blend-add");
        // (0.5, 0.75, 0.25) added onto a (0.25, 0.5, 0) clear → (0.75, 1.25→1.0, 0.25).
        const auto px = draw_over(
            pipe, {0.25f, 0.5f, 0.0f, 1.0f}, {0.5f, 0.75f, 0.25f, 1.0f}, Format::RGBA8Unorm, 4);
        const std::size_t c = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
        CHECK(std::abs(static_cast<int>(px[c + 0]) - 191) <= 2); // 0.75
        CHECK(px[c + 1] == 255);                                 // saturated at 1.0
        CHECK(std::abs(static_cast<int>(px[c + 2]) - 64) <= 2);  // 0.25
        device->destroy(pipe);
    }

    SUBCASE("RGBA16Float keeps radiance above 1.0") {
        const PipelineHandle pipe =
            make_pushconst_pipeline(BlendMode::None, Format::RGBA16Float, "hdr-target");
        // 2.25 would clamp to 1.0 in UNORM8; in float16 it must come back exactly (integer+quarter
        // values this small are exactly representable in binary16).
        const auto px = draw_over(
            pipe, {0.0f, 0.0f, 0.0f, 1.0f}, {0.25f, 1.0f, 2.25f, 1.0f}, Format::RGBA16Float, 8);
        const std::size_t c = (static_cast<std::size_t>(size / 2) * size + size / 2) * 8;
        const auto ch = [&](std::size_t i) {
            const std::uint16_t h =
                static_cast<std::uint16_t>(px[c + 2 * i] | (px[c + 2 * i + 1] << 8));
            return half_to_float(h);
        };
        CHECK(ch(0) == doctest::Approx(0.25f));
        CHECK(ch(1) == doctest::Approx(1.0f));
        CHECK(ch(2) == doctest::Approx(2.25f)); // survived past UNORM's ceiling
        device->destroy(pipe);
    }

    SUBCASE("one pass writes two render targets") {
        ShaderDesc mfd{};
        mfd.stage = ShaderStage::Fragment;
        mfd.spirv = mrt_frag_spv;
        mfd.spirv_size_bytes = sizeof(mrt_frag_spv);
        mfd.debug_name = "mrt.frag";
        const ShaderHandle mfsh = device->create_shader(mfd);

        const Format formats[] = {Format::RGBA8Unorm, Format::RGBA8Unorm};
        GraphicsPipelineDesc pd{};
        pd.vertex_shader = vsh;
        pd.fragment_shader = mfsh;
        pd.color_formats = formats; // the MRT declaration (wins over color_format)
        pd.topology = PrimitiveTopology::TriangleList;
        pd.cull = CullMode::None;
        pd.debug_name = "mrt-pipeline";
        const PipelineHandle pipe = device->create_graphics_pipeline(pd);

        const auto make_target = [&](const char* name) {
            TextureDesc td{};
            td.extent = {size, size};
            td.format = Format::RGBA8Unorm;
            td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
            td.debug_name = name;
            return device->create_texture(td);
        };
        const TextureHandle target_a = make_target("mrt-a");
        const TextureHandle target_b = make_target("mrt-b");

        const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
        const auto make_readback = [&](const char* name) {
            BufferDesc rbd{};
            rbd.size = bytes;
            rbd.usage = BufferUsage::TransferDst;
            rbd.memory = MemoryUsage::GpuToCpu;
            rbd.debug_name = name;
            return device->create_buffer(rbd);
        };
        const BufferHandle rb_a = make_readback("mrt-readback-a");
        const BufferHandle rb_b = make_readback("mrt-readback-b");

        auto cmd = device->begin_commands();
        const ColorAttachment atts[] = {
            {target_a, LoadOp::Clear, StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}},
            {target_b, LoadOp::Clear, StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}},
        };
        RenderingInfo ri{};
        ri.colors = atts;
        cmd->begin_rendering(ri);
        cmd->bind_pipeline(pipe);
        Viewport vp{};
        vp.width = static_cast<float>(size);
        vp.height = static_cast<float>(size);
        vp.max_depth = 1.0f;
        cmd->set_viewport(vp);
        Rect2D sc{};
        sc.width = size;
        sc.height = size;
        cmd->set_scissor(sc);
        cmd->draw(3); // ONE draw fills BOTH images
        cmd->end_rendering();
        cmd->copy_texture_to_buffer(target_a, rb_a);
        cmd->copy_texture_to_buffer(target_b, rb_b);
        device->submit_blocking(*cmd);

        std::vector<std::uint8_t> px_a(bytes), px_b(bytes);
        device->read_buffer(rb_a, px_a.data(), px_a.size(), 0);
        device->read_buffer(rb_b, px_b.data(), px_b.size(), 0);

        const std::size_t c = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
        CHECK(px_a[c + 0] == 255); // attachment 0 got orange (1, .5, 0)
        CHECK(std::abs(static_cast<int>(px_a[c + 1]) - 128) <= 2);
        CHECK(px_a[c + 2] == 0);
        CHECK(px_b[c + 0] == 0); // attachment 1 got cyan (0, .5, 1)
        CHECK(std::abs(static_cast<int>(px_b[c + 1]) - 128) <= 2);
        CHECK(px_b[c + 2] == 255);

        device->destroy(rb_b);
        device->destroy(rb_a);
        device->destroy(target_b);
        device->destroy(target_a);
        device->destroy(pipe);
        device->destroy(mfsh);
    }

    device->destroy(fsh);
    device->destroy(vsh);
}
