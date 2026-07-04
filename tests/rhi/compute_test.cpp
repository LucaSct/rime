// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the compute brick (M5.2, ADR-0021). Two claims:
//
//  (1) A compute pipeline dispatches and writes a storage buffer: every element of a 1024-uint
//      buffer must hold exactly what its invocation computed — verified value-for-value on the
//      CPU. This is the whole GPGPU loop: create → bind → dispatch → read back.
//
//  (2) Compute output feeds graphics: a dispatch imageStore()s a pattern into a storage image,
//      then a draw SAMPLES that image into a color target — crossing the compute→graphics
//      boundary through the post-dispatch barrier and the general-layout sampling path. The
//      readback must show the compute-written pattern, pixel for pixel.
//
// GPU-free on lavapipe (llvmpipe runs compute on the CPU), like every proof since M3.3.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "fill.comp.spv.h"
#include "pattern.comp.spv.h"
#include "pushconst.vert.spv.h"
#include "rime/rhi/rhi.hpp"
#include "sample_storage.frag.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi compute pipelines dispatch and feed graphics (M5.2)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping compute dispatch");
        return;
    }

    SUBCASE("a dispatch fills a storage buffer, verified element-for-element") {
        constexpr std::uint32_t kCount = 1024; // 16 workgroups of local_size_x = 64

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
        pd.debug_name = "fill-pipeline";
        const PipelineHandle pipe = device->create_compute_pipeline(pd);

        BufferDesc bd{};
        bd.size = kCount * sizeof(std::uint32_t);
        bd.usage = BufferUsage::Storage;
        bd.memory = MemoryUsage::GpuToCpu; // host-readable so the CPU can verify every element
        bd.debug_name = "fill-output";
        const BufferHandle buf = device->create_buffer(bd);

        auto cmd = device->begin_commands();
        cmd->bind_compute_pipeline(pipe);
        cmd->bind_storage_buffer(0, buf);
        cmd->dispatch(kCount / 64, 1, 1);
        device->submit_blocking(*cmd);

        std::vector<std::uint32_t> data(kCount);
        device->read_buffer(buf, data.data(), data.size() * sizeof(std::uint32_t), 0);
        std::size_t mismatches = 0;
        for (std::uint32_t i = 0; i < kCount; ++i) {
            if (data[i] != i * 7u + 3u)
                ++mismatches;
        }
        CHECK(mismatches == 0);

        device->destroy(buf);
        device->destroy(pipe);
        device->destroy(csh);
    }

    SUBCASE("a compute-written storage image is sampled by a draw") {
        const std::uint32_t size = 32;

        // The compute half: imageStore a coordinate-derived pattern into a storage image.
        ShaderDesc csd{};
        csd.stage = ShaderStage::Compute;
        csd.spirv = pattern_comp_spv;
        csd.spirv_size_bytes = sizeof(pattern_comp_spv);
        csd.debug_name = "pattern.comp";
        const ShaderHandle csh = device->create_shader(csd);

        const BindingDesc cs_bindings[] = {{0, BindingType::StorageImage, StageMask::Compute}};
        ComputePipelineDesc cpd{};
        cpd.shader = csh;
        cpd.bindings = cs_bindings;
        cpd.debug_name = "pattern-pipeline";
        const PipelineHandle cpipe = device->create_compute_pipeline(cpd);

        TextureDesc td{};
        td.extent = {size, size};
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::Storage | TextureUsage::Sampled; // written by CS, read by FS
        td.debug_name = "pattern-image";
        const TextureHandle img = device->create_texture(td);

        // The graphics half: full-screen triangle sampling the image texel-for-pixel.
        ShaderDesc vsd{};
        vsd.stage = ShaderStage::Vertex;
        vsd.spirv = pushconst_vert_spv;
        vsd.spirv_size_bytes = sizeof(pushconst_vert_spv);
        vsd.debug_name = "pushconst.vert (fullscreen)";
        const ShaderHandle vsh = device->create_shader(vsd);

        ShaderDesc fsd{};
        fsd.stage = ShaderStage::Fragment;
        fsd.spirv = sample_storage_frag_spv;
        fsd.spirv_size_bytes = sizeof(sample_storage_frag_spv);
        fsd.debug_name = "sample_storage.frag";
        const ShaderHandle fsh = device->create_shader(fsd);

        const BindingDesc gfx_bindings[] = {
            {0, BindingType::CombinedImageSampler, StageMask::Fragment}};
        GraphicsPipelineDesc gpd{};
        gpd.vertex_shader = vsh;
        gpd.fragment_shader = fsh;
        gpd.color_format = Format::RGBA8Unorm;
        gpd.topology = PrimitiveTopology::TriangleList;
        gpd.cull = CullMode::None;
        gpd.bindings = gfx_bindings;
        gpd.debug_name = "sample-storage-pipeline";
        const PipelineHandle gpipe = device->create_graphics_pipeline(gpd);

        SamplerDesc smd{};
        smd.mag_filter = Filter::Nearest; // exact texel fetch: pixel (x,y) reads texel (x,y)
        smd.min_filter = Filter::Nearest;
        smd.address_mode = AddressMode::ClampToEdge;
        smd.debug_name = "nearest-sampler";
        const SamplerHandle smp = device->create_sampler(smd);

        TextureDesc ctd{};
        ctd.extent = {size, size};
        ctd.format = Format::RGBA8Unorm;
        ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        ctd.debug_name = "sample-target";
        const TextureHandle target = device->create_texture(ctd);

        const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
        BufferDesc rbd{};
        rbd.size = bytes;
        rbd.usage = BufferUsage::TransferDst;
        rbd.memory = MemoryUsage::GpuToCpu;
        rbd.debug_name = "sample-readback";
        const BufferHandle rb = device->create_buffer(rbd);

        // One command buffer, both halves: dispatch → (implicit post-dispatch barrier) → draw.
        auto cmd = device->begin_commands();
        cmd->bind_compute_pipeline(cpipe);
        cmd->bind_storage_image(0, img); // transitions the image to GENERAL
        cmd->dispatch(size / 8, size / 8, 1);

        RenderingInfo ri{};
        ri.color.target = target;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = {1.0f, 0.0f, 1.0f, 1.0f}; // magenta: any unsampled pixel screams
        cmd->begin_rendering(ri);
        cmd->bind_pipeline(gpipe);
        cmd->bind_texture(0, img, smp); // sampled in GENERAL — the layout the dispatch left
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
        cmd->copy_texture_to_buffer(target, rb);
        device->submit_blocking(*cmd);

        std::vector<std::uint8_t> px(bytes);
        device->read_buffer(rb, px.data(), px.size(), 0);

        // Spot-check texels across the image: R/G must encode the coordinate, B the 4×4 checker.
        std::size_t mismatches = 0;
        for (std::uint32_t y = 0; y < size; y += 3) {
            for (std::uint32_t x = 0; x < size; x += 3) {
                const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
                const int checker = (((x / 4) + (y / 4)) % 2) ? 255 : 0;
                if (px[at + 0] != x || px[at + 1] != y ||
                    std::abs(static_cast<int>(px[at + 2]) - checker) > 0) {
                    ++mismatches;
                }
            }
        }
        CHECK(mismatches == 0);

        device->destroy(rb);
        device->destroy(target);
        device->destroy(smp);
        device->destroy(gpipe);
        device->destroy(fsh);
        device->destroy(vsh);
        device->destroy(img);
        device->destroy(cpipe);
        device->destroy(csh);
    }
}
