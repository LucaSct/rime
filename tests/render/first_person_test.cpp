// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>

#include "first_person_depth_probe.frag.spv.h"
#include "fullscreen.vert.spv.h"
#include "render_test_support.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
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
    LightingSettings ssr{};
    ssr.ssr_enabled = true;
    renderer.set_lighting(ssr);
    const auto with_ssr = render(true);
    const auto channel = [](const test::HdrImage& image, std::uint32_t x, std::uint32_t y, int c) {
        return image.rgb[(y * kSize + x) * 3 + c];
    };
    CHECK(channel(only_world, 32, 32, 0) > channel(only_world, 32, 32, 2) + 0.1f);
    CHECK(channel(with_model, 32, 32, 2) > channel(with_model, 32, 32, 0) + 0.1f);
    CHECK(channel(with_model, 2, 32, 0) ==
          doctest::Approx(channel(only_world, 2, 32, 0)).epsilon(0.02));
    CHECK(channel(off_again, 32, 32, 0) > channel(off_again, 32, 32, 2) + 0.1f);
    CHECK(channel(with_ssr, 32, 32, 2) > channel(with_ssr, 32, 32, 0) + 0.1f);
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
    core::Transform background{};
    background.translation = {0.0f, 0.0f, -6.0f};
    background.scale = {10.0f, 10.0f, 1.0f};
    (void)world.spawn_with(ecs::WorldTransform{background}, MeshRef{slab}, MaterialRef{mat});
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_taa_resolve_enabled(false);
    renderer.set_temporal_jitter_enabled(false);
    DrawItem held{};
    held.mesh = slab;
    held.material = mat;
    held.index_count = meshes.get(slab).index_count;
    const auto render = [&](core::Transform camera_tf, core::Mat4 model) {
        world.get<ecs::WorldTransform>(camera)->value = camera_tf;
        held.model = model;
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
        return std::array<float, 2>{test::half_to_float(half[(32 * kSize + 32) * 2]),
                                    test::half_to_float(half[(32 * kSize + 5) * 2])};
    };
    core::Transform pose{};
    (void)render(pose, core::mat4_translation({0.0f, 0.0f, -3.0f}));
    pose.translation.x = 0.5f;
    const auto attached =
        render(pose, core::to_matrix(pose) * core::mat4_translation({0.0f, 0.0f, -3.0f}));
    const auto recoiled = render(pose, core::mat4_translation({0.7f, 0.0f, -3.0f}));
    CHECK(std::abs(attached[0]) < 0.002f);
    CHECK(std::abs(attached[1]) > 0.01f); // parent world velocity survived the model LOAD pass
    CHECK(std::abs(recoiled[0]) > 0.01f);
    pose.rotation = core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.15f);
    const auto rotated =
        render(pose, core::to_matrix(pose) * core::mat4_translation({0.2f, 0.0f, -3.0f}));
    CHECK(std::abs(rotated[0]) < 0.002f);
}

TEST_CASE("first-person metal reads filtered sky with DDGI while covered world metal stays dark") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device exists");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const MeshId slab = meshes.add(make_box({0.5f, 0.5f, 0.005f}), "ddgi-metal-slab");
    PbrMaterialDesc metal{};
    metal.base_color[0] = metal.base_color[1] = metal.base_color[2] = 0.9f;
    metal.metallic = 1.0f;
    metal.roughness = 0.8f;
    const MaterialId mat = materials.add(metal);
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    core::Transform wall{};
    wall.translation = {0.0f, 0.0f, -2.0f};
    wall.scale = {5.0f, 5.0f, 1.0f};
    (void)world.spawn_with(ecs::WorldTransform{wall}, MeshRef{slab}, MaterialRef{mat});
    DrawItem held{};
    held.mesh = slab;
    held.material = mat;
    held.model = core::mat4_translation({0.0f, 0.0f, -3.0f});
    held.index_count = meshes.get(slab).index_count;
    LightingSettings lighting{};
    lighting.sdf_clipmap_enabled = true;
    lighting.ddgi_enabled = true;
    lighting.ddgi_probe_count_x = 2;
    lighting.ddgi_probe_count_y = 2;
    lighting.ddgi_probe_count_z = 2;
    lighting.ddgi_rays_per_probe = 16;
    SkyParams sky{};
    sky.enabled = true;
    sky.clouds_enabled = false;
    sky.use_scene_sun = false;
    sky.sun_direction[0] = 0.0f;
    sky.sun_direction[1] = 0.8f;
    sky.sun_direction[2] = -0.6f;
    const auto render = [&](bool filtered) {
        SceneRenderer renderer(*device, meshes, materials);
        test::disable_temporal_aa(renderer);
        renderer.set_ambient(0.0f, 0.0f, 0.0f);
        renderer.set_lighting(lighting);
        renderer.set_sky(sky);
        renderer.set_sky_specular_prefilter_enabled(filtered);
        RenderGraph graph(*device);
        graph.reset();
        renderer.set_first_person_draws(std::span<const DrawItem>(&held, 1), 1);
        const auto out = renderer.render(graph, world, {kSize, kSize}, true);
        CHECK(out.sky_prefiltered.is_valid() == filtered);
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const auto image = test::decode_hdr(
            test::read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
        CHECK(renderer.sky_specular_stats().prefilter_filled == (filtered ? 1 : 0));
        return std::array<float, 2>{image.luminance(32, 32), image.luminance(5, 32)};
    };
    const auto legacy = render(false);
    const auto filtered = render(true);
    MESSAGE("DDGI metal: legacy model=" << legacy[0] << " filtered model=" << filtered[0]
                                        << " world=" << filtered[1]);
    CHECK(filtered[0] > 0.01f);
    CHECK(filtered[0] > 5.0f * filtered[1]);
    CHECK(std::abs(filtered[0] - legacy[0]) > 0.001f);
}

TEST_CASE("first-person draw scratch remains isolated across three queued frames") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device exists");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const MeshId slab = meshes.add(make_box({0.5f, 0.5f, 0.005f}), "queued-slab");
    const std::array<MaterialId, 3> mats = {materials.add({{1.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 1.0f}),
                                            materials.add({{0.0f, 1.0f, 0.0f, 1.0f}, 0.0f, 1.0f}),
                                            materials.add({{0.0f, 0.0f, 1.0f, 1.0f}, 0.0f, 1.0f})};
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    SceneRenderer renderer(*device, meshes, materials);
    test::disable_temporal_aa(renderer);
    renderer.set_ambient(0.8f, 0.8f, 0.8f);
    std::array<std::unique_ptr<RenderGraph>, 3> graphs;
    std::array<RGTexture, 3> outputs{};
    std::array<rhi::SubmitTicket, 3> tickets{};
    for (std::size_t frame = 0; frame < graphs.size(); ++frame) {
        graphs[frame] = std::make_unique<RenderGraph>(*device);
        graphs[frame]->set_frames_in_flight(2);
        graphs[frame]->reset();
        DrawItem held{};
        held.mesh = slab;
        held.material = mats[frame];
        held.model = core::mat4_translation({0.0f, 0.0f, -3.0f});
        held.index_count = meshes.get(slab).index_count;
        renderer.set_first_person_draws(std::span<const DrawItem>(&held, 1), frame + 1);
        const auto out = renderer.render(*graphs[frame], world, {kSize, kSize}, true);
        graphs[frame]->export_texture(out.hdr);
        outputs[frame] = out.hdr;
        auto cmd = device->begin_commands();
        graphs[frame]->execute(*cmd);
        tickets[frame] = device->submit(std::move(cmd)); // intentionally no wait here
        REQUIRE(tickets[frame].is_valid());
    }
    for (const auto ticket : tickets)
        device->wait(ticket);
    for (std::size_t frame = 0; frame < graphs.size(); ++frame) {
        const auto image = test::decode_hdr(
            test::read_texture(*device, graphs[frame]->physical(outputs[frame]), kSize, kSize, 8),
            kSize,
            kSize);
        const std::size_t center = (32 * kSize + 32) * 3;
        const float own = image.rgb[center + frame];
        const float other = image.rgb[center + (frame + 1) % 3];
        CHECK(own > other + 0.2f);
    }
}

TEST_CASE("first-person appearance, swap and disable reject parent TAA history") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device exists");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const MeshId slab = meshes.add(make_box({0.5f, 0.5f, 0.005f}), "taa-overlay-slab");
    const MaterialId world_mat = materials.add({{0.25f, 0.25f, 0.25f, 1.0f}, 0.0f, 1.0f});
    const MaterialId blue = materials.add({{0.2f, 0.3f, 1.0f, 1.0f}, 0.0f, 1.0f});
    const MaterialId green = materials.add({{0.2f, 0.9f, 0.3f, 1.0f}, 0.0f, 1.0f});
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    core::Transform wall{};
    wall.translation = {0.0f, 0.0f, -2.0f};
    wall.scale = {4.0f, 4.0f, 1.0f};
    (void)world.spawn_with(ecs::WorldTransform{wall}, MeshRef{slab}, MaterialRef{world_mat});
    SceneRenderer raw(*device, meshes, materials);
    SceneRenderer taa(*device, meshes, materials);
    raw.set_taa_resolve_enabled(false);
    for (SceneRenderer* renderer : {&raw, &taa})
        renderer->set_ambient(0.8f, 0.8f, 0.8f);
    DrawItem held{};
    held.mesh = slab;
    held.model = core::mat4_translation({0.0f, 0.0f, -3.0f});
    held.index_count = meshes.get(slab).index_count;
    auto frame =
        [&](SceneRenderer& renderer, bool visible, MaterialId material, std::uint64_t epoch) {
            held.material = material;
            RenderGraph graph(*device);
            graph.reset();
            if (visible)
                renderer.set_first_person_draws(std::span<const DrawItem>(&held, 1), epoch);
            const auto out = renderer.render(graph, world, {kSize, kSize}, true);
            graph.export_texture(out.hdr);
            auto cmd = device->begin_commands();
            graph.execute(*cmd);
            device->submit_blocking(*cmd);
            return test::read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8);
        };
    auto pair = [&](bool visible, MaterialId material, std::uint64_t epoch) {
        const auto a = frame(raw, visible, material, epoch);
        const auto b = frame(taa, visible, material, epoch);
        CHECK(taa.last_frame_resolved());
        return std::pair{a, b};
    };
    (void)pair(false, blue, 0);
    (void)pair(false, blue, 0);
    const auto appeared = pair(true, blue, 1);
    CHECK(appeared.first == appeared.second);
    const auto settled_blue = pair(true, blue, 1);
    CHECK(settled_blue.first != settled_blue.second); // history actually contributes at edges
    const auto swapped = pair(true, green, 2);
    CHECK(swapped.first == swapped.second);
    const auto settled_green = pair(true, green, 2);
    CHECK(settled_green.first != settled_green.second);
    const auto disabled = pair(false, green, 2);
    CHECK(disabled.first == disabled.second);
}
