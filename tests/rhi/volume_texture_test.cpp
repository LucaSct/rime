// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for ADR-0013 — 3-D (volume) textures. It uploads a 1×1×2 RGBA32F volume (a red slice at
// w=0, a green slice at w=1), then draws a full-screen triangle whose fragment shader samples the
// volume down its depth (w) axis by screen-y. Reading the image back, the top half must be red and
// the bottom half green — which can only happen if the 3-D image, the 3-D image view, the
// depth-aware staged upload, and the `sampler3D` descriptor all worked end to end. Off-screen +
// readback, so it runs on a software GPU (lavapipe) in CI with no display. (main() for this exe is
// in device_test.cpp.)

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "volume.frag.spv.h"
#include "volume.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi samples a 3-D (volume) texture off-screen (pixel-verified)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping volume render");
        return;
    }

    // A 1×1×2 volume: slice 0 (w=0) red, slice 1 (w=1) green. RGBA32F, depth-major, tightly packed.
    const std::array<float, 8> voxels = {{1.0f,
                                          0.0f,
                                          0.0f,
                                          1.0f, // w=0 → red
                                          0.0f,
                                          1.0f,
                                          0.0f,
                                          1.0f}}; // w=1 → green
    TextureDesc vtd{};
    vtd.extent = {1, 1};
    vtd.depth = 2; // → a 3-D image
    vtd.format = Format::RGBA32Float;
    vtd.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    vtd.debug_name = "volume-3d";
    const TextureHandle volume = device->create_texture(vtd);
    device->write_texture(volume, voxels.data(), voxels.size() * sizeof(float));

    SamplerDesc smd{};
    smd.mag_filter = Filter::Nearest; // crisp halves: each slice is one solid color, easy to assert
    smd.min_filter = Filter::Nearest;
    smd.address_mode = AddressMode::ClampToEdge;
    smd.debug_name = "volume-sampler";
    const SamplerHandle sampler = device->create_sampler(smd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = volume_vert_spv;
    vsd.spirv_size_bytes = sizeof(volume_vert_spv);
    const ShaderHandle vsh = device->create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = volume_frag_spv;
    fsd.spirv_size_bytes = sizeof(volume_frag_spv);
    const ShaderHandle fsh = device->create_shader(fsd);

    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vsh;
    pd.fragment_shader =
        fsh; // no vertex layout: the full-screen triangle is generated from the index
    pd.color_format = Format::RGBA8Unorm;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.cull = CullMode::None;
    pd.sampled_texture = true; // set 0 / binding 0 = the sampler3D
    pd.debug_name = "volume-pipeline";
    const PipelineHandle pipeline = device->create_graphics_pipeline(pd);

    const std::uint32_t size = 32;
    TextureDesc ctd{};
    ctd.extent = {size, size};
    ctd.format = Format::RGBA8Unorm;
    ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    ctd.debug_name = "volume-target";
    const TextureHandle color = device->create_texture(ctd);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "volume-readback";
    const BufferHandle readback = device->create_buffer(rbd);

    auto cmd = device->begin_commands();
    {
        RenderingInfo ri{};
        ri.color.target = color;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->begin_rendering(ri);
        cmd->bind_pipeline(pipeline);
        cmd->bind_texture(0, volume, sampler);
        Viewport vp{};
        vp.width = static_cast<float>(size);
        vp.height = static_cast<float>(size);
        vp.max_depth = 1.0f;
        cmd->set_viewport(vp);
        Rect2D sc{};
        sc.width = size;
        sc.height = size;
        cmd->set_scissor(sc);
        cmd->draw(3); // full-screen triangle
        cmd->end_rendering();
    }
    cmd->copy_texture_to_buffer(color, readback);
    device->submit_blocking(*cmd);

    std::vector<std::uint8_t> px(byte_count);
    device->read_buffer(readback, px.data(), px.size(), 0);

    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return &px[(static_cast<std::size_t>(y) * size + x) * 4];
    };
    // Top of the image samples w≈0 (red slice); bottom samples w≈1 (green slice).
    const std::uint8_t* top = at(size / 2, size / 8);
    const std::uint8_t* bottom = at(size / 2, size - size / 8);
    CHECK(top[0] > 128); // top is red
    CHECK(top[1] < 128);
    CHECK(bottom[1] > 128); // bottom is green
    CHECK(bottom[0] < 128);

    device->destroy(readback);
    device->destroy(color);
    device->destroy(pipeline);
    device->destroy(fsh);
    device->destroy(vsh);
    device->destroy(sampler);
    device->destroy(volume);
}
