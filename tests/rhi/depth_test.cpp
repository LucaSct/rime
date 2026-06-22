// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the depth-attachment brick (A1) — depth testing, made deterministic and GPU-free in CI.
// It renders two overlapping triangles that cover the image center at different depths and colors:
//   near (z = 0.25) green, submitted FIRST; far (z = 0.75) red, submitted SECOND.
// The exact same geometry, in the exact same order, is drawn twice — the only difference is whether a
// depth buffer is attached and tested:
//   * depth ON  → the nearer (green) triangle wins even though red was drawn last  ⇒ center is GREEN.
//   * depth OFF → painter's order wins, so the last-drawn (red) triangle covers it ⇒ center is RED.
// That color flip at the center is an unambiguous proof the depth test is doing its job (and the
// depth-OFF control proves the geometry really overlaps and submission order would otherwise decide).
// Off-screen + readback, so it runs on a software GPU (lavapipe) in CI. (main() is in device_test.cpp.)

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"

// The compiled depth shaders, embedded by rime_add_shaders (see tests/rhi/CMakeLists.txt).
#include "depth.frag.spv.h"
#include "depth.vert.spv.h"

namespace {

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

struct Vertex {
    float x, y, z; // NDC position (z in [0,1], near..far)
    float r, g, b; // color
};

// Two triangles that both cover the center (0,0) but not the corners, at two depths/colors. Near is
// listed first so it is *drawn* first — the order the depth test must override.
constexpr Vertex kVerts[] = {
    {-0.7f, -0.7f, 0.25f, 0.0f, 1.0f, 0.0f}, // near (green)
    {0.7f, -0.7f, 0.25f, 0.0f, 1.0f, 0.0f},
    {0.0f, 0.7f, 0.25f, 0.0f, 1.0f, 0.0f},
    {-0.7f, -0.7f, 0.75f, 1.0f, 0.0f, 0.0f}, // far (red)
    {0.7f, -0.7f, 0.75f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.7f, 0.75f, 1.0f, 0.0f, 0.0f},
};

} // namespace

TEST_CASE("rhi depth test: nearer fragments win regardless of draw order") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping depth render");
        return;
    }

    const std::uint32_t size = 64;
    const ClearColor clear{0.1f, 0.1f, 0.3f, 1.0f}; // dark blue

    // Shared resources: one vertex buffer + the two shaders.
    BufferDesc vbd{};
    vbd.size = sizeof(kVerts);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu; // host-visible: initial_data uploads directly, no staging
    vbd.initial_data = kVerts;
    vbd.debug_name = "depth-verts";
    const BufferHandle vbuf = device->create_buffer(vbd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = depth_vert_spv;
    vsd.spirv_size_bytes = sizeof(depth_vert_spv);
    vsd.debug_name = "depth.vert";
    const ShaderHandle vsh = device->create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = depth_frag_spv;
    fsd.spirv_size_bytes = sizeof(depth_frag_spv);
    fsd.debug_name = "depth.frag";
    const ShaderHandle fsh = device->create_shader(fsd);

    static const VertexAttribute attrs[] = {
        {0, Format::RGB32Float, 0},                 // position at byte offset 0
        {1, Format::RGB32Float, sizeof(float) * 3}, // color at byte offset 12
    };

    // Two pipelines identical except for the depth state — that is the whole experiment.
    const auto make_pipeline = [&](bool depth) {
        GraphicsPipelineDesc pd{};
        pd.vertex_shader = vsh;
        pd.fragment_shader = fsh;
        pd.vertex_layout.stride = sizeof(Vertex);
        pd.vertex_layout.attributes = attrs;
        pd.color_format = Format::RGBA8Unorm;
        pd.topology = PrimitiveTopology::TriangleList;
        pd.cull = CullMode::None;
        pd.depth_test = depth;
        pd.depth_write = depth;
        pd.depth_compare = CompareOp::Less;
        if (depth) pd.depth_format = Format::D32Float;
        pd.debug_name = depth ? "depth-on-pipeline" : "depth-off-pipeline";
        return device->create_graphics_pipeline(pd);
    };
    const PipelineHandle pipe_on = make_pipeline(true);
    const PipelineHandle pipe_off = make_pipeline(false);

    // One color target per pass + a single shared depth buffer (only the depth-ON pass attaches it).
    const auto make_color = [&](const char* name) {
        TextureDesc td{};
        td.extent = {size, size};
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        td.debug_name = name;
        return device->create_texture(td);
    };
    const TextureHandle color_on = make_color("depth-color-on");
    const TextureHandle color_off = make_color("depth-color-off");

    TextureDesc dtd{};
    dtd.extent = {size, size};
    dtd.format = Format::D32Float;
    dtd.usage = TextureUsage::DepthStencil;
    dtd.debug_name = "depth-buffer";
    const TextureHandle depth_tex = device->create_texture(dtd);

    const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
    const auto make_readback = [&](const char* name) {
        BufferDesc rbd{};
        rbd.size = bytes;
        rbd.usage = BufferUsage::TransferDst;
        rbd.memory = MemoryUsage::GpuToCpu;
        rbd.debug_name = name;
        return device->create_buffer(rbd);
    };
    const BufferHandle rb_on = make_readback("depth-readback-on");
    const BufferHandle rb_off = make_readback("depth-readback-off");

    // Record one pass: clear, then draw near (verts 0..2) then far (verts 3..5).
    const auto record_pass =
        [&](CommandBuffer& cmd, PipelineHandle pipe, TextureHandle color, bool with_depth) {
            RenderingInfo ri{};
            ri.color.target = color;
            ri.color.load_op = LoadOp::Clear;
            ri.color.store_op = StoreOp::Store;
            ri.color.clear = clear;
            if (with_depth) {
                DepthStencilAttachment da{};
                da.target = depth_tex;
                da.load_op = LoadOp::Clear;
                da.clear_depth = 1.0f; // far plane
                ri.depth_stencil = da;
            }
            cmd.begin_rendering(ri);
            cmd.bind_pipeline(pipe);
            cmd.bind_vertex_buffer(vbuf, 0);

            Viewport vp{};
            vp.width = static_cast<float>(size);
            vp.height = static_cast<float>(size);
            vp.max_depth = 1.0f;
            cmd.set_viewport(vp);
            Rect2D sc{};
            sc.width = size;
            sc.height = size;
            cmd.set_scissor(sc);

            cmd.draw(3, 1, 0, 0); // near (green), drawn first
            cmd.draw(3, 1, 3, 0); // far (red), drawn second
            cmd.end_rendering();
        };

    auto cmd = device->begin_commands();
    record_pass(*cmd, pipe_on, color_on, /*with_depth=*/true);
    record_pass(*cmd, pipe_off, color_off, /*with_depth=*/false);
    cmd->copy_texture_to_buffer(color_on, rb_on);
    cmd->copy_texture_to_buffer(color_off, rb_off);
    device->submit_blocking(*cmd);

    std::vector<std::uint8_t> px_on(bytes), px_off(bytes);
    device->read_buffer(rb_on, px_on.data(), px_on.size(), 0);
    device->read_buffer(rb_off, px_off.data(), px_off.size(), 0);

    const auto center = [&](const std::vector<std::uint8_t>& px) -> const std::uint8_t* {
        return &px[(static_cast<std::size_t>(size / 2) * size + size / 2) * 4];
    };

    // Depth ON: the nearer (green) triangle wins even though red was drawn last.
    const std::uint8_t* c_on = center(px_on);
    CHECK(c_on[1] > 180); // G high
    CHECK(c_on[0] < 80);  // R low

    // Depth OFF (control): same draws, last-drawn (red) wins — proving the triangles really overlap
    // and that, without depth, submission order alone decides the pixel.
    const std::uint8_t* c_off = center(px_off);
    CHECK(c_off[0] > 180); // R high
    CHECK(c_off[1] < 80);  // G low

    device->destroy(rb_off);
    device->destroy(rb_on);
    device->destroy(depth_tex);
    device->destroy(color_off);
    device->destroy(color_on);
    device->destroy(pipe_off);
    device->destroy(pipe_on);
    device->destroy(fsh);
    device->destroy(vsh);
    device->destroy(vbuf);
}
