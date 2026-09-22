// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "triangle.frag.spv.h"
#include "triangle.vert.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

struct Vertex {
    float x, y;
    float r, g, b;
};

struct IndexedIndirectCommand {
    std::uint32_t index_count;
    std::uint32_t instance_count;
    std::uint32_t first_index;
    std::int32_t vertex_offset;
    std::uint32_t first_instance;
};

static_assert(sizeof(IndexedIndirectCommand) == 20);
} // namespace

TEST_CASE("rhi draws indexed geometry from two indirect commands") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping indexed indirect render");
        return;
    }

    static constexpr Vertex vertices[] = {
        {-0.9f, -0.8f, 1.0f, 0.0f, 0.0f},
        {-0.1f, -0.8f, 1.0f, 0.0f, 0.0f},
        {-0.5f, 0.8f, 1.0f, 0.0f, 0.0f},
        {0.1f, -0.8f, 0.0f, 1.0f, 0.0f},
        {0.9f, -0.8f, 0.0f, 1.0f, 0.0f},
        {0.5f, 0.8f, 0.0f, 1.0f, 0.0f},
    };
    static constexpr std::uint32_t indices[] = {0, 1, 2, 3, 4, 5};
    static constexpr IndexedIndirectCommand commands[] = {
        {3, 1, 0, 0, 0},
        {3, 1, 3, 0, 0},
    };

    BufferDesc vbd{};
    vbd.size = sizeof(vertices);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu;
    vbd.initial_data = vertices;
    const BufferHandle vertex_buffer = device->create_buffer(vbd);

    BufferDesc ibd{};
    ibd.size = sizeof(indices);
    ibd.usage = BufferUsage::Index;
    ibd.memory = MemoryUsage::CpuToGpu;
    ibd.initial_data = indices;
    const BufferHandle index_buffer = device->create_buffer(ibd);

    BufferDesc cbd{};
    cbd.size = sizeof(commands);
    cbd.usage = BufferUsage::Indirect;
    cbd.memory = MemoryUsage::CpuToGpu;
    cbd.initial_data = commands;
    const BufferHandle indirect_buffer = device->create_buffer(cbd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = triangle_vert_spv;
    vsd.spirv_size_bytes = sizeof(triangle_vert_spv);
    const ShaderHandle vertex_shader = device->create_shader(vsd);
    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = triangle_frag_spv;
    fsd.spirv_size_bytes = sizeof(triangle_frag_spv);
    const ShaderHandle fragment_shader = device->create_shader(fsd);

    static constexpr VertexAttribute attributes[] = {
        {0, Format::RG32Float, 0},
        {1, Format::RGB32Float, sizeof(float) * 2},
    };
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader;
    pd.fragment_shader = fragment_shader;
    pd.vertex_layout = {sizeof(Vertex), attributes};
    pd.color_format = Format::RGBA8Unorm;
    pd.cull = CullMode::None;
    const PipelineHandle pipeline = device->create_graphics_pipeline(pd);

    TextureDesc td{};
    td.extent = {64, 64};
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    const TextureHandle color = device->create_texture(td);
    BufferDesc rbd{};
    rbd.size = 64 * 64 * 4;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    const BufferHandle readback = device->create_buffer(rbd);

    auto cmd = device->begin_commands();
    RenderingInfo rendering{};
    rendering.color.target = color;
    rendering.color.clear = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->begin_rendering(rendering);
    cmd->bind_pipeline(pipeline);
    cmd->bind_vertex_buffer(vertex_buffer);
    cmd->bind_index_buffer(index_buffer, IndexType::Uint32);
    cmd->set_viewport({0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f});
    cmd->set_scissor({0, 0, 64, 64});
    cmd->draw_indexed_indirect(indirect_buffer, 2);
    cmd->end_rendering();
    cmd->copy_texture_to_buffer(color, readback);
    device->submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(rbd.size);
    device->read_buffer(readback, pixels.data(), pixels.size());
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return &pixels[(static_cast<std::size_t>(y) * 64 + x) * 4];
    };
    CHECK(pixel(16, 32)[0] > 180);
    CHECK(pixel(48, 32)[1] > 180);
    CHECK(pixel(32, 4)[0] == 0);
    CHECK(pixel(32, 4)[1] == 0);

    device->destroy(readback);
    device->destroy(color);
    device->destroy(pipeline);
    device->destroy(fragment_shader);
    device->destroy(vertex_shader);
    device->destroy(indirect_buffer);
    device->destroy(index_buffer);
    device->destroy(vertex_buffer);
}
