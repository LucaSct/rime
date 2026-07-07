// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M6.3's RHI top-up: Device::write_texture_mips uploads a PRE-GENERATED mip chain
// verbatim, one buffer→image copy per level, instead of GPU-blitting the coarse levels from level 0
// the way write_texture does. Cooked textures carry gamma-correct offline mips (linear-space box
// filter), and the engine must not throw them away and regenerate a (wrong, too-dark) chain on
// upload.
//
// The trick is a chain whose coarse levels are DELIBERATELY TINTED to colours a box filter of level
// 0 could never produce: level 0 red, level 1 green, level 2 blue. Sampling each explicit LOD (via
// textureLod, the M5.3 minify shader) must read back that level's tint — green at LOD 1, blue at
// LOD 2 — which can only be true if the per-level bytes were uploaded. For contrast, a second
// texture gets the same red level 0 through write_texture (blit generation): its LOD 1 comes back
// red, not green, so the two upload paths visibly diverge. Off-screen + readback, GPU-free on
// lavapipe in CI. (main() for this exe is in device_test.cpp.)

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "minify_lod.frag.spv.h"
#include "pushconst.vert.spv.h"
#include "rime/rhi/rhi.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// A solid w×h RGBA8 level (row-major), every texel the given colour.
std::vector<std::byte> solid(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 4> rgba) {
    std::vector<std::byte> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = std::byte{rgba[0]};
        px[i + 1] = std::byte{rgba[1]};
        px[i + 2] = std::byte{rgba[2]};
        px[i + 3] = std::byte{rgba[3]};
    }
    return px;
}
} // namespace

TEST_CASE("rhi uploads a pre-generated (cooked) mip chain verbatim (M6.3)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping cooked-mip upload proof");
        return;
    }

    // A 4×4 texture has a 3-level chain: 4×4, 2×2, 1×1. Tint each level distinctly.
    const std::uint32_t tex_size = 4;
    const std::vector<std::byte> level0 = solid(4, 4, {255, 0, 0, 255}); // red
    const std::vector<std::byte> level1 = solid(2, 2, {0, 255, 0, 255}); // green
    const std::vector<std::byte> level2 = solid(1, 1, {0, 0, 255, 255}); // blue

    TextureDesc td{};
    td.extent = {tex_size, tex_size};
    td.mip_levels = 3;
    td.format =
        Format::RGBA8Unorm; // linear: tints pass through sampling unchanged, exact to assert
    td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    td.debug_name = "cooked-tinted-chain";
    const TextureHandle cooked = device->create_texture(td);

    // The path under test: upload the whole tinted chain, one MipData span per level.
    const MipData levels[] = {
        {std::span<const std::byte>(level0)},
        {std::span<const std::byte>(level1)},
        {std::span<const std::byte>(level2)},
    };
    device->write_texture_mips(cooked, levels);

    // Contrast texture: the same red level 0 through write_texture, which blit-generates the chain
    // — every level ends up red (a downsampled solid colour is the same colour).
    td.debug_name = "blit-generated-chain";
    const TextureHandle blit = device->create_texture(td);
    device->write_texture(blit, level0.data(), level0.size());

    SamplerDesc smd{};
    smd.mag_filter = Filter::Nearest;
    smd.min_filter = Filter::Nearest;
    smd.mip_filter = Filter::Nearest; // explicit integer LODs → exactly one level per fetch
    smd.address_mode = AddressMode::ClampToEdge;
    smd.debug_name = "cooked-mip-nearest";
    const SamplerHandle smp = device->create_sampler(smd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = pushconst_vert_spv;
    vsd.spirv_size_bytes = sizeof(pushconst_vert_spv);
    vsd.debug_name = "pushconst.vert (fullscreen)";
    const ShaderHandle vsh = device->create_shader(vsd);
    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = minify_lod_frag_spv;
    fsd.spirv_size_bytes = sizeof(minify_lod_frag_spv);
    fsd.debug_name = "minify_lod.frag";
    const ShaderHandle fsh = device->create_shader(fsd);

    const BindingDesc bindings[] = {{0, BindingType::CombinedImageSampler, StageMask::Fragment}};
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vsh;
    pd.fragment_shader = fsh;
    pd.color_format = Format::RGBA8Unorm;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None;
    pd.bindings = bindings;
    pd.push_constant_size = sizeof(float); // the explicit LOD
    pd.debug_name = "cooked-mip-pipeline";
    const PipelineHandle pipe = device->create_graphics_pipeline(pd);

    const std::uint32_t out_size = 4; // matches the shader's gl_FragCoord.xy / 4.0
    const std::uint64_t bytes = static_cast<std::uint64_t>(out_size) * out_size * 4;

    // Render the fullscreen triangle sampling `tex` at an explicit LOD, read back the centre texel.
    const auto sample_at_lod = [&](TextureHandle tex, float lod) -> std::array<std::uint8_t, 4> {
        TextureDesc otd{};
        otd.extent = {out_size, out_size};
        otd.format = Format::RGBA8Unorm;
        otd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        otd.debug_name = "cooked-mip-target";
        const TextureHandle target = device->create_texture(otd);
        BufferDesc rbd{};
        rbd.size = bytes;
        rbd.usage = BufferUsage::TransferDst;
        rbd.memory = MemoryUsage::GpuToCpu;
        rbd.debug_name = "cooked-mip-readback";
        const BufferHandle rb = device->create_buffer(rbd);

        auto cmd = device->begin_commands();
        RenderingInfo ri{};
        ri.color.target = target;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        cmd->begin_rendering(ri);
        cmd->bind_pipeline(pipe);
        cmd->bind_texture(0, tex, smp);
        cmd->push_constants(&lod, sizeof(float));
        Viewport vp{};
        vp.width = static_cast<float>(out_size);
        vp.height = static_cast<float>(out_size);
        vp.max_depth = 1.0f;
        cmd->set_viewport(vp);
        Rect2D sc{};
        sc.width = out_size;
        sc.height = out_size;
        cmd->set_scissor(sc);
        cmd->draw(3);
        cmd->end_rendering();
        cmd->copy_texture_to_buffer(target, rb);
        device->submit_blocking(*cmd);

        std::vector<std::uint8_t> px(bytes);
        device->read_buffer(rb, px.data(), px.size(), 0);
        device->destroy(rb);
        device->destroy(target);
        const std::size_t c =
            (static_cast<std::size_t>(out_size / 2) * out_size + out_size / 2) * 4;
        return {px[c + 0], px[c + 1], px[c + 2], px[c + 3]};
    };

    // Each channel is a pure 0 or 255 and we point-sample a solid level, so >128 recovers the tint.
    const auto is_rgb = [](std::array<std::uint8_t, 4> p, bool r, bool g, bool b) {
        return (p[0] > 128) == r && (p[1] > 128) == g && (p[2] > 128) == b;
    };

    // The cooked chain reads back each level's own tint — only possible if write_texture_mips
    // copied the per-level bytes rather than regenerating them.
    const auto c0 = sample_at_lod(cooked, 0.0f);
    const auto c1 = sample_at_lod(cooked, 1.0f);
    const auto c2 = sample_at_lod(cooked, 2.0f);
    CHECK(is_rgb(c0, true, false, false)); // LOD 0 → red
    CHECK(is_rgb(c1, false, true, false)); // LOD 1 → green (the cooked tint, not a blit of red)
    CHECK(is_rgb(c2, false, false, true)); // LOD 2 → blue

    // The blit path regenerates level 1 from the red level 0, so it comes back red — the two upload
    // paths diverge exactly where it matters, which is the whole point of the per-mip path.
    const auto b1 = sample_at_lod(blit, 1.0f);
    CHECK(is_rgb(b1, true, false, false)); // blit LOD 1 → red
    CHECK(c1 != b1);                       // cooked (green) ≠ blit-generated (red)

    device->destroy(pipe);
    device->destroy(fsh);
    device->destroy(vsh);
    device->destroy(smp);
    device->destroy(blit);
    device->destroy(cooked);
}
