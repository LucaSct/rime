// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>

#include "first_person_depth_probe.frag.spv.h"
#include "fullscreen.vert.spv.h"
#include "render_test_support.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/first_person_pass.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"

namespace {
using namespace rime;
using namespace rime::render;

constexpr std::uint32_t kSize = 64;

bool has_pass(const RenderGraph& graph, std::string_view name) {
    for (std::size_t i = 0; i < graph.pass_count(); ++i)
        if (graph.pass_name(static_cast<std::uint32_t>(i)) == name)
            return true;
    return false;
}

} // namespace

TEST_CASE(
    "first-person depth selects model even behind nearer world, and preserves uncovered world") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device exists");
        return;
    }
    rhi::ShaderDesc sd{};
    sd.stage = rhi::ShaderStage::Vertex;
    sd.spirv = fullscreen_vert_spv;
    sd.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    const auto vs = device->create_shader(sd);
    sd.stage = rhi::ShaderStage::Fragment;
    sd.spirv = first_person_depth_probe_frag_spv;
    sd.spirv_size_bytes = sizeof(first_person_depth_probe_frag_spv);
    const auto fs = device->create_shader(sd);
    const rhi::BindingDesc binding[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}};
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vs;
    pd.fragment_shader = fs;
    pd.color_format = kHdrFormat;
    pd.cull = rhi::CullMode::None;
    pd.bindings = binding;
    const auto pipeline = device->create_graphics_pipeline(pd);
    rhi::SamplerDesc sampler_desc{};
    sampler_desc.mag_filter = rhi::Filter::Nearest;
    sampler_desc.min_filter = rhi::Filter::Nearest;
    const auto sampler = device->create_sampler(sampler_desc);
    FirstPersonPass pass(*device);

    const auto probe = [&](float model_clear) {
        RenderGraph graph(*device);
        graph.reset();
        const auto world = graph.create_texture({{kSize, kSize}, kDepthFormat, "test-world-depth"});
        const auto model = graph.create_texture({{kSize, kSize}, kDepthFormat, "test-model-depth"});
        const auto color = graph.create_texture({{kSize, kSize}, kHdrFormat, "depth-probe"});
        auto clear = [&](RGTexture texture, float value, std::string_view label) {
            RGDepthAttachment depth{};
            depth.texture = texture;
            depth.load = rhi::LoadOp::Clear;
            depth.store = rhi::StoreOp::Store;
            depth.clear_depth = value;
            RenderGraph::RasterPassDesc desc{};
            desc.depth = &depth;
            graph.add_raster_pass(label, desc, [](rhi::CommandBuffer&) {});
        };
        clear(world, 0.2f, "clear-near-world");
        clear(model, model_clear, "clear-model");
        const auto combined = pass.combine_depth(graph, world, model, {kSize, kSize});
        const RGColorAttachment output[] = {{color, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}}};
        const RGTexture sampled[] = {combined};
        RenderGraph::RasterPassDesc desc{};
        desc.colors = output;
        desc.sampled = sampled;
        graph.add_raster_pass("depth-probe", desc, [&, combined](rhi::CommandBuffer& cmd) {
            cmd.bind_pipeline(pipeline);
            cmd.bind_texture(0, graph.physical(combined), sampler);
            cmd.draw(3);
        });
        graph.export_texture(color);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const auto bytes = test::read_texture(*device, graph.physical(color), kSize, kSize, 8);
        const auto image = test::decode_hdr(bytes, kSize, kSize);
        return image.rgb[(kSize * kSize / 2) * 3];
    };
    CHECK(probe(0.8f) == doctest::Approx(0.8f).epsilon(0.002));
    CHECK(probe(1.0f) == doctest::Approx(0.2f).epsilon(0.002));
    device->destroy(sampler);
    device->destroy(pipeline);
    device->destroy(fs);
    device->destroy(vs);
}

TEST_CASE("first-person overlay loads world HDR and is off unless explicitly supplied") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device exists");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const MeshId slab = meshes.add(make_box({0.5f, 0.5f, 0.005f}), "first-person-slab");
    const MaterialId red = materials.add({{1.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 1.0f});
    const MaterialId blue = materials.add({{0.0f, 0.0f, 1.0f, 1.0f}, 0.0f, 1.0f});
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    core::Transform wall{};
    wall.translation = {0.0f, 0.0f, -1.0f};
    wall.scale = {3.0f, 3.0f, 1.0f};
    (void)world.spawn_with(ecs::WorldTransform{wall}, MeshRef{slab}, MaterialRef{red});
    SceneRenderer renderer(*device, meshes, materials);
    test::disable_temporal_aa(renderer);
    renderer.set_ambient(0.5f, 0.5f, 0.5f);

    DrawItem held{};
    held.mesh = slab;
    held.material = blue;
    held.model = core::mat4_translation({0.0f, 0.0f, -3.0f});
    held.first_index = 0;
    held.index_count = static_cast<std::uint32_t>(meshes.get(slab).index_count);
    auto render = [&](bool overlay) {
        RenderGraph graph(*device);
        graph.reset();
        if (overlay)
            renderer.set_first_person_draws(std::span<const DrawItem>(&held, 1), 1);
        const auto out = renderer.render(graph, world, {kSize, kSize}, true);
        CHECK(has_pass(graph, "first-person-forward") == overlay);
        CHECK(has_pass(graph, "first-person-combine-depth") == overlay);
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return test::decode_hdr(
            test::read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    };
    const auto only_world = render(false);
    const auto with_model = render(true);
    const auto off_again = render(false);
    const auto channel = [](const test::HdrImage& image, std::uint32_t x, std::uint32_t y, int c) {
        return image.rgb[(y * kSize + x) * 3 + c];
    };
    CHECK(channel(only_world, 32, 32, 0) > channel(only_world, 32, 32, 2) + 0.1f);
    CHECK(channel(with_model, 32, 32, 2) > channel(with_model, 32, 32, 0) + 0.1f);
    CHECK(channel(with_model, 2, 32, 0) ==
          doctest::Approx(channel(only_world, 2, 32, 0)).epsilon(0.02));
    CHECK(channel(off_again, 32, 32, 0) > channel(off_again, 32, 32, 2) + 0.1f);
}

TEST_CASE("first-person velocity cancels camera attachment motion but detects pose change") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device exists");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const MeshId slab = meshes.add(make_box({0.5f, 0.5f, 0.005f}), "velocity-slab");
    const MaterialId mat = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 1.0f});
    ecs::World world;
    register_render_components(world);
    const auto camera = world.spawn_with(ecs::WorldTransform{}, Camera{});
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_taa_resolve_enabled(false);
    renderer.set_temporal_jitter_enabled(false);
    DrawItem held{};
    held.mesh = slab;
    held.material = mat;
    held.index_count = meshes.get(slab).index_count;
    const auto render = [&](float camera_x, float model_x) {
        world.get<ecs::WorldTransform>(camera)->value.translation.x = camera_x;
        held.model = core::mat4_translation({model_x, 0.0f, -3.0f});
        RenderGraph graph(*device);
        graph.reset();
        renderer.set_first_person_draws(std::span<const DrawItem>(&held, 1), 7);
        const auto out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.velocity.is_valid());
        graph.export_texture(out.velocity);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const auto bytes =
            test::read_texture(*device, graph.physical(out.velocity), kSize, kSize, 4);
        const auto* half = reinterpret_cast<const std::uint16_t*>(bytes.data());
        return test::half_to_float(half[(32 * kSize + 32) * 2]);
    };
    (void)render(0.0f, 0.0f);
    const float attached = render(0.5f, 0.5f);
    const float recoiled = render(0.5f, 0.7f);
    CHECK(std::abs(attached) < 0.002f);
    CHECK(std::abs(recoiled) > 0.01f);
}
