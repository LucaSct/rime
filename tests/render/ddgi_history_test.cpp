// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ddgi_remap.comp.spv.h"
#include "render_test_support.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/render_graph.hpp"

using namespace rime;
using namespace rime::render;

namespace {
struct Params {
    std::int32_t shift[4]{};
    std::uint32_t dims[4]{3, 3, 2, 9};
};

void prove_gpu_remap(rhi::Device& dev, std::array<int, 3> shift, bool full_reset) {
    constexpr std::uint32_t total = 18;
    rhi::ShaderDesc sd{};
    sd.stage = rhi::ShaderStage::Compute;
    sd.spirv = ddgi_remap_comp_spv;
    sd.spirv_size_bytes = sizeof(ddgi_remap_comp_spv);
    auto shader = dev.create_shader(sd);
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {1, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {2, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {3, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
        {4, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
    };
    rhi::ComputePipelineDesc pd{};
    pd.shader = shader;
    pd.bindings = bindings;
    auto pipeline = dev.create_compute_pipeline(pd);

    std::array<rhi::TextureHandle, 4> textures{};
    std::array<std::vector<std::uint8_t>, 2> seeded;
    for (std::uint32_t atlas = 0; atlas < 2; ++atlas) {
        const std::uint32_t tile = atlas == 0 ? 8 : 16;
        const std::uint32_t width = 9 * tile;
        const std::uint32_t height = 2 * tile;
        rhi::TextureDesc td{};
        td.extent = {width, height};
        td.format = atlas == 0 ? rhi::Format::RGBA16Float : rhi::Format::RG32Float;
        td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::TransferDst |
                   rhi::TextureUsage::TransferSrc;
        textures[atlas] = dev.create_texture(td);
        textures[atlas + 2] = dev.create_texture(td);
        auto& bytes = seeded[atlas];
        bytes.resize(static_cast<std::size_t>(width) * height * 8);
        for (std::uint32_t probe = 0; probe < total; ++probe) {
            for (std::uint32_t y = 0; y < tile; ++y) {
                for (std::uint32_t x = 0; x < tile; ++x) {
                    const auto offset = (static_cast<std::size_t>(probe / 9 * tile + y) * width +
                                         probe % 9 * tile + x) *
                                        8;
                    if (atlas == 0) {
                        std::uint16_t rgba[4] = {
                            static_cast<std::uint16_t>(0x3c00 + probe * 64 + y * tile + x),
                            static_cast<std::uint16_t>(0x4000 + probe),
                            static_cast<std::uint16_t>(0x4400 + y * tile + x),
                            0x3c00};
                        std::memcpy(bytes.data() + offset, rgba, 8);
                    } else {
                        float moments[2] = {static_cast<float>(1000 * probe + 16 * y + x),
                                            static_cast<float>(2000 * probe + 32 * y + 2 * x)};
                        std::memcpy(bytes.data() + offset, moments, 8);
                    }
                }
            }
        }
        dev.write_texture(textures[atlas], bytes.data(), bytes.size());
    }

    RenderGraph graph(dev);
    graph.reset();
    const RGTexture src_i = graph.import_texture(textures[0], rhi::ResourceState::ShaderRead);
    const RGTexture src_v = graph.import_texture(textures[1], rhi::ResourceState::ShaderRead);
    const RGTexture dst_i = graph.import_texture(textures[2], rhi::ResourceState::Undefined);
    const RGTexture dst_v = graph.import_texture(textures[3], rhi::ResourceState::Undefined);
    Params params{};
    params.shift[0] = shift[0];
    params.shift[1] = shift[1];
    params.shift[2] = shift[2];
    params.shift[3] = full_reset ? 1 : 0;
    const auto slice = graph.push_frame_data(&params, sizeof(params));
    const RGTexture reads[] = {src_i, src_v};
    const RGTexture writes[] = {dst_i, dst_v};
    RenderGraph::ComputePassDesc desc{};
    desc.storage_read = reads;
    desc.storage_write = writes;
    graph.add_compute_pass("ddgi-remap-proof", desc, [=](rhi::CommandBuffer& cmd) {
        cmd.bind_compute_pipeline(pipeline);
        for (std::uint32_t i = 0; i < 4; ++i)
            cmd.bind_storage_image(i, textures[i]);
        cmd.bind_uniform_buffer(4, slice.buffer, slice.offset, slice.size);
        cmd.dispatch(1, 1, total);
    });
    auto cmd = dev.begin_commands();
    graph.execute(*cmd);
    dev.submit_blocking(*cmd);

    for (std::uint32_t atlas = 0; atlas < 2; ++atlas) {
        const std::uint32_t tile = atlas == 0 ? 8 : 16;
        const std::uint32_t width = 9 * tile;
        const auto actual = test::read_texture(dev, textures[atlas + 2], width, 2 * tile, 8);
        for (int qz = 0; qz < 2; ++qz)
            for (int qy = 0; qy < 3; ++qy)
                for (int qx = 0; qx < 3; ++qx) {
                    const int sx = qx + shift[0], sy = qy + shift[1], sz = qz + shift[2];
                    const bool survives =
                        !full_reset && sx >= 0 && sx < 3 && sy >= 0 && sy < 3 && sz >= 0 && sz < 2;
                    const int dst = qx + 3 * qy + 9 * qz;
                    const int src = sx + 3 * sy + 9 * sz;
                    for (std::uint32_t y = 0; y < tile; ++y)
                        for (std::uint32_t x = 0; x < tile; ++x) {
                            const auto dest_offset =
                                (static_cast<std::size_t>(dst / 9 * tile + y) * width +
                                 dst % 9 * tile + x) *
                                8;
                            const auto src_offset =
                                (static_cast<std::size_t>(src / 9 * tile + y) * width +
                                 src % 9 * tile + x) *
                                8;
                            const std::uint8_t zeros[8]{};
                            CHECK(std::memcmp(actual.data() + dest_offset,
                                              survives ? seeded[atlas].data() + src_offset : zeros,
                                              8) == 0);
                        }
                }
    }
    for (auto texture : textures)
        dev.destroy(texture);
    dev.destroy(pipeline);
    dev.destroy(shader);
}
} // namespace

TEST_CASE("ddgi history: GPU remap preserves both complete atlas tiles and clears exposed tiles") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan device required for DDGI remap proof");
        return;
    }
    prove_gpu_remap(*device, {1, 0, 0}, false);
    prove_gpu_remap(*device, {-1, 0, 0}, false);
    prove_gpu_remap(*device, {1, -1, 1}, false);
    prove_gpu_remap(*device, {-2, 1, -1}, false);
    prove_gpu_remap(*device, {0, 0, 0}, true);
}
