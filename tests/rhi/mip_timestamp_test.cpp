// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the mipmaps + timestamps brick (M5.3). Three claims:
//
//  (1) write_texture generates a real mip chain: a red/blue 4-texel checker sampled at explicit
//      LOD 1 is still a pure block color (fine levels preserve blocks), while LOD 3 — where each
//      texel is the box-filtered average of 8×8 base texels spanning two whole blocks — is an
//      even red/blue mix. Pure→mixed across levels can only come from generated, progressively
//      filtered mips.
//
//  (2) Anisotropic sampler creation works (feature-gated, clamped) — exercised, not visually
//      asserted: aniso quality needs a ground-plane-at-grazing-angle scene, which arrives with
//      first light; here we prove the creation path and that sampling through it is well-formed.
//
//  (3) GPU timestamps measure real work: two stamps around a dispatch read back monotonic,
//      nonzero nanoseconds (skipped gracefully where a device cannot timestamp — lavapipe can).
//      Debug labels bracket the work (no-op without VK_EXT_debug_utils; must never crash).
//
// Off-screen + readback, GPU-free on lavapipe in CI (the M3.3 proof pattern).

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "fill.comp.spv.h"
#include "minify_lod.frag.spv.h"
#include "pushconst.vert.spv.h"
#include "rime/rhi/rhi.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi mip generation and GPU timestamps (M5.3)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping mip/timestamp proof");
        return;
    }

    SUBCASE("the generated chain averages across blocks as levels coarsen") {
        const std::uint32_t tex_size = 32; // full chain: 32→16→8→4→2→1 (6 levels)

        // Level 0: vertical red/blue stripes, 4 texels wide.
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(tex_size) * tex_size * 4);
        for (std::uint32_t y = 0; y < tex_size; ++y) {
            for (std::uint32_t x = 0; x < tex_size; ++x) {
                const bool red = ((x / 4) % 2) == 0;
                const std::size_t at = (static_cast<std::size_t>(y) * tex_size + x) * 4;
                pixels[at + 0] = red ? 255 : 0;
                pixels[at + 1] = 0;
                pixels[at + 2] = red ? 0 : 255;
                pixels[at + 3] = 255;
            }
        }

        TextureDesc td{};
        td.extent = {tex_size, tex_size};
        td.mip_levels = 6; // ask for the full chain
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
        td.debug_name = "mip-stripes";
        const TextureHandle tex = device->create_texture(td);
        device->write_texture(tex, pixels.data(), pixels.size()); // level 0 + generated chain

        SamplerDesc smd{};
        smd.mag_filter = Filter::Nearest; // point sampling: the assert reads exact texels
        smd.min_filter = Filter::Nearest;
        smd.mip_filter = Filter::Nearest; // explicit integer LODs → a single level per fetch
        smd.address_mode = AddressMode::ClampToEdge;
        smd.max_anisotropy = 8.0f; // (2) the aniso creation path, feature-gated + clamped
        smd.debug_name = "mip-nearest-aniso";
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

        const BindingDesc bindings[] = {
            {0, BindingType::CombinedImageSampler, StageMask::Fragment}};
        GraphicsPipelineDesc pd{};
        pd.vertex_shader = vsh;
        pd.fragment_shader = fsh;
        pd.color_format = Format::RGBA8Unorm;
        pd.topology = PrimitiveTopology::TriangleList;
        pd.cull = CullMode::None;
        pd.bindings = bindings;
        pd.push_constant_size = sizeof(float); // the explicit LOD
        pd.debug_name = "minify-lod-pipeline";
        const PipelineHandle pipe = device->create_graphics_pipeline(pd);

        const std::uint32_t out_size = 4;
        const std::uint64_t bytes = static_cast<std::uint64_t>(out_size) * out_size * 4;

        const auto sample_at_lod = [&](float lod) {
            TextureDesc otd{};
            otd.extent = {out_size, out_size};
            otd.format = Format::RGBA8Unorm;
            otd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
            otd.debug_name = "minify-target";
            const TextureHandle target = device->create_texture(otd);
            BufferDesc rbd{};
            rbd.size = bytes;
            rbd.usage = BufferUsage::TransferDst;
            rbd.memory = MemoryUsage::GpuToCpu;
            rbd.debug_name = "minify-readback";
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
            return px;
        };

        // LOD 1 (16×16): each texel is a 2×2 box INSIDE one 4-wide stripe — still a pure color.
        // Every 4×4-target sample lands on an odd level-1 texel (coord 4x+2), i.e. a blue stripe.
        const auto lod1 = sample_at_lod(1.0f);
        // LOD 3 (4×4): each texel averages 8×8 base texels = one full red + one full blue stripe.
        const auto lod3 = sample_at_lod(3.0f);

        const std::size_t c =
            (static_cast<std::size_t>(out_size / 2) * out_size + out_size / 2) * 4;
        CHECK(lod1[c + 0] < 30); // pure stripe color at a fine level…
        CHECK(lod1[c + 2] > 200);
        CHECK(lod3[c + 0] > 90);  // …but an even mix at the coarse level: the box-filtered
        CHECK(lod3[c + 0] < 170); // average of both stripes. Only a generated chain does this.
        CHECK(lod3[c + 2] > 90);
        CHECK(lod3[c + 2] < 170);

        device->destroy(pipe);
        device->destroy(fsh);
        device->destroy(vsh);
        device->destroy(smp);
        device->destroy(tex);
    }

    SUBCASE("timestamps around a dispatch are monotonic nonzero nanoseconds") {
        ShaderDesc csd{};
        csd.stage = ShaderStage::Compute;
        csd.spirv = fill_comp_spv;
        csd.spirv_size_bytes = sizeof(fill_comp_spv);
        csd.debug_name = "fill.comp";
        const ShaderHandle csh = device->create_shader(csd);
        const BindingDesc bindings[] = {{0, BindingType::StorageBuffer, StageMask::Compute}};
        ComputePipelineDesc pd{};
        pd.shader = csh;
        pd.bindings = bindings;
        pd.debug_name = "fill-pipeline (timed)";
        const PipelineHandle pipe = device->create_compute_pipeline(pd);

        BufferDesc bd{};
        bd.size = 4096 * sizeof(std::uint32_t);
        bd.usage = BufferUsage::Storage;
        bd.memory = MemoryUsage::GpuOnly;
        bd.debug_name = "timed-fill-output";
        const BufferHandle buf = device->create_buffer(bd);

        auto cmd = device->begin_commands();
        cmd->begin_debug_label("timed-fill"); // (3) labels: no-op without debug utils, never UB
        cmd->write_timestamp(0);
        cmd->bind_compute_pipeline(pipe);
        cmd->bind_storage_buffer(0, buf);
        cmd->dispatch(4096 / 64, 1, 1);
        cmd->write_timestamp(1);
        cmd->end_debug_label();
        device->submit_blocking(*cmd);

        std::array<std::uint64_t, 2> ns{};
        if (!cmd->read_timestamps(ns)) {
            MESSAGE("device cannot timestamp — skipping (documented degrade)");
        } else {
            CHECK(ns[0] > 0);
            CHECK(ns[1] > ns[0]); // the dispatch took a nonzero, forward amount of GPU time
        }

        device->destroy(buf);
        device->destroy(pipe);
        device->destroy(csh);
    }
}
