// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>

#include "render_test_support.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/render/scope_lens_pass.hpp"

namespace {
using namespace rime;
using namespace rime::render;
constexpr std::uint32_t kSize = 64;

CpuMesh lens_disc() {
    CpuMesh mesh;
    mesh.vertices.push_back({});
    constexpr std::uint32_t segments = 64;
    for (std::uint32_t i = 0; i <= segments; ++i) {
        const float a = static_cast<float>(i) * 6.28318530718f / segments;
        MeshVertex vertex{};
        vertex.px = 0.5f * std::cos(a);
        vertex.py = 0.5f * std::sin(a);
        vertex.u = 0.5f + vertex.px;
        vertex.v = 0.5f + vertex.py;
        mesh.vertices.push_back(vertex);
        if (i) {
            mesh.indices.push_back(0);
            mesh.indices.push_back(i);
            mesh.indices.push_back(i + 1);
        }
    }
    return mesh;
}

float channel(const test::HdrImage& image, unsigned x, unsigned y, unsigned c) {
    return image.rgb[(y * kSize + x) * 3 + c];
}

struct LensFixture {
    rhi::Device& device;
    MeshRegistry meshes;
    ScopeLensPass lens_pass;
    DepthPrepass depth_pass;
    DrawItem lens;
    DrawItem housing;
    rhi::TextureHandle environment;
    rhi::TextureHandle optical_input;
    ScopeLensPass::Params params;

    explicit LensFixture(rhi::Device& d) : device(d), meshes(d), lens_pass(d), depth_pass(d) {
        lens.mesh = meshes.add(lens_disc(), "scope-test-disc");
        lens.index_count = meshes.get(lens.mesh).index_count;
        lens.model = core::mat4_translation({0.0f, 0.0f, 0.5f});
        lens.flags = DrawItem::DoubleSided;
        housing.mesh = meshes.add(make_box({0.15f, 0.5f, 0.01f}), "scope-test-housing");
        housing.index_count = meshes.get(housing.mesh).index_count;
        housing.model = core::mat4_translation({-0.22f, 0.0f, 0.25f});
        params.optic_from_world = core::inverse(lens.model);
        params.main_eye = {0.0f, 0.0f, 0.59f};
        params.lens_radius = 0.5f;
        params.optical_view_proj =
            core::perspective(2.0f * std::atan(0.04f), 1.0f, 0.01f, 1000.0f) *
            params.optic_from_world;
        params.reticle_strength = 0.0f;
    }

    ~LensFixture() { device.destroy(environment); }

    test::HdrImage render(rhi::ClearColor scope_color,
                          bool occlusion = false,
                          bool valid_scope = true,
                          RGTexture* result = nullptr) {
        RenderGraph graph(device);
        graph.reset();
        const auto hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "parent-hdr"});
        const auto depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "model-depth"});
        const auto scope = optical_input.is_valid()
                               ? graph.import_texture(optical_input, rhi::ResourceState::ShaderRead)
                               : graph.create_texture({{kSize, kSize}, kHdrFormat, "optical-hdr"});
        const auto clear_color =
            [&](RGTexture texture, rhi::ClearColor color, std::string_view label) {
                const RGColorAttachment attachment[] = {
                    {texture, rhi::LoadOp::Clear, rhi::StoreOp::Store, color}};
                RenderGraph::RasterPassDesc desc{};
                desc.colors = attachment;
                graph.add_raster_pass(label, desc, [](rhi::CommandBuffer&) {});
            };
        clear_color(hdr, {0.125f, 0.25f, 0.5f, 1.0f}, "parent-clear");
        if (!optical_input.is_valid())
            clear_color(scope, scope_color, "scope-clear");

        // The lens itself must already be in model depth, exactly as in first-person rendering.
        const std::array<DrawItem, 2> draws{lens, housing};
        GpuFrameUniforms frame{};
        frame.view_proj = params.main_view_proj;
        const auto frame_slice = graph.push_frame_data(&frame, sizeof(frame));
        std::array<std::byte, 2 * kDrawUniformStride> draw_bytes{};
        for (unsigned i = 0; i < 2; ++i) {
            GpuDrawUniforms draw{};
            draw.model = draws[i].model;
            std::memcpy(draw_bytes.data() + i * kDrawUniformStride, &draw, sizeof(draw));
        }
        SceneDrawData data{};
        data.meshes = &meshes;
        data.draws = std::span(draws.data(), occlusion ? 2 : 1);
        data.bind_frame_ubo(frame_slice.buffer, frame_slice.offset, frame_slice.size);
        // Draw uniform offsets are relative to the buffer start; allocate it separately so its
        // first slice starts at zero (the production scene renderer owns its own per-draw ring).
        data.draw_ubo = graph.push_frame_buffer(draw_bytes.data(), draw_bytes.size());
        depth_pass.add(graph, depth, data);
        const auto sky = environment.is_valid()
                             ? graph.import_texture(environment, rhi::ResourceState::ShaderRead)
                             : RGTexture{};
        const auto output = lens_pass.add(graph,
                                          hdr,
                                          depth,
                                          valid_scope ? scope : RGTexture{},
                                          sky,
                                          {kSize, kSize},
                                          meshes,
                                          lens,
                                          params);
        if (result)
            *result = output;
        graph.export_texture(output);
        auto commands = device.begin_commands();
        graph.execute(*commands);
        device.submit_blocking(*commands);
        return test::decode_hdr(
            test::read_texture(device, graph.physical(output), kSize, kSize, 8), kSize, kSize);
    }
};
} // namespace

TEST_CASE("scope image changes physical lens pixels while parent exterior remains identical") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan required for scope lens proof");
        return;
    }
    LensFixture fixture(*device);
    const auto red = fixture.render({1.0f, 0.0f, 0.0f, 1.0f});
    const auto green = fixture.render({0.0f, 1.0f, 0.0f, 1.0f});
    unsigned changed_inside = 0;
    for (unsigned y = 0; y < kSize; ++y) {
        for (unsigned x = 0; x < kSize; ++x) {
            const float dx = (float(x) + 0.5f - 32.0f) / 16.0f;
            const float dy = (float(y) + 0.5f - 32.0f) / 16.0f;
            if (dx * dx + dy * dy > 1.01f) {
                for (unsigned c = 0; c < 3; ++c)
                    CHECK(channel(red, x, y, c) == channel(green, x, y, c));
                CHECK(channel(red, x, y, 0) == 0.125f);
            } else if (std::abs(channel(red, x, y, 0) - channel(green, x, y, 0)) > 0.1f) {
                ++changed_inside;
            }
        }
    }
    CHECK(changed_inside > 300);
    CHECK(channel(red, 32, 32, 0) > 0.8f);
    CHECK(channel(green, 32, 32, 1) > 0.8f);
    const auto blocked = fixture.render({1.0f, 0.0f, 0.0f, 1.0f}, true);
    CHECK(channel(blocked, 25, 32, 0) == 0.125f);
    CHECK(channel(blocked, 37, 32, 0) > 0.5f);
}

TEST_CASE("unmagnified sight preserves screen-space landmarks and remains transparent off-axis") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan required for sight glass proof");
        return;
    }
    LensFixture fixture(*device);
    rhi::TextureDesc desc{};
    desc.extent = {kSize, kSize};
    desc.format = rhi::Format::RGBA8Unorm;
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    fixture.optical_input = device->create_texture(desc);
    std::array<std::uint8_t, kSize * kSize * 4> pixels{};
    for (unsigned y = 0; y < kSize; ++y)
        for (unsigned x = 0; x < kSize; ++x) {
            const auto i = (y * kSize + x) * 4;
            pixels[i] = static_cast<std::uint8_t>(x * 4);
            pixels[i + 1] = static_cast<std::uint8_t>(y * 4);
            pixels[i + 2] = 64;
            pixels[i + 3] = 255;
        }
    device->write_texture(fixture.optical_input, pixels.data(), pixels.size());
    fixture.params.unmagnified = true;
    fixture.params.reflection_strength = 0;
    fixture.params.main_eye.x = .3f; // a magnifying scope's exit pupil is entirely closed here
    const auto clear = fixture.render({});
    for (const auto x : {29u, 37u}) {
        // Fresnel is a scalar: channel ratios remove it and expose a shifted/flipped/scaled
        // transmission image. This asymmetric gradient can detect all three errors.
        const float r_over_b = channel(clear, x, 32, 0) / channel(clear, x, 32, 2);
        const float g_over_b = channel(clear, x, 32, 1) / channel(clear, x, 32, 2);
        CHECK(r_over_b == doctest::Approx((x / 16.0f) * .965f / .975f).epsilon(.01));
        CHECK(g_over_b == doctest::Approx(2.0f * .985f / .975f).epsilon(.01));
    }
    CHECK(channel(clear, 0, 0, 0) == .125f);
    const auto blocked = fixture.render({}, true);
    CHECK(channel(blocked, 25, 32, 0) == .125f);
    fixture.params.unmagnified = false;
    const auto scope = fixture.render({});
    CHECK(channel(scope, 37, 32, 2) == 0);
    device->destroy(fixture.optical_input);
}

TEST_CASE("scope glass reflection responds to control, normal and eye; eye box blacks out") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan required for scope lens proof");
        return;
    }
    LensFixture fixture(*device);
    fixture.params.reflection_strength = 0.0f;
    const auto off = fixture.render({});
    fixture.params.reflection_strength = 1.0f;
    const auto on = fixture.render({});
    CHECK(channel(off, 32, 32, 0) == 0.0f);
    CHECK(channel(on, 32, 32, 0) > 0.001f);
    // Feed the same 2-D-array view used by the real GGX sky chain. A direction-varying
    // source detects an ignored sky binding or an incorrect reflected direction.
    rhi::TextureDesc sky{};
    sky.extent = {8, 8};
    sky.array_view = true;
    sky.format = rhi::Format::RGBA8Unorm;
    sky.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    fixture.environment = device->create_texture(sky);
    std::array<std::uint8_t, 8 * 8 * 4> pixels{};
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            pixels[(y * 8 + x) * 4] = static_cast<std::uint8_t>(x * 32);
            pixels[(y * 8 + x) * 4 + 2] = 128;
            pixels[(y * 8 + x) * 4 + 3] = 255;
        }
    device->write_texture(fixture.environment, pixels.data(), pixels.size());
    const auto sky_on = fixture.render({});
    CHECK(channel(sky_on, 32, 32, 0) > channel(on, 32, 32, 0) * 5.0f);
    auto tilted_mesh = lens_disc();
    for (auto& vertex : tilted_mesh.vertices) {
        vertex.nx = 0.8f;
        vertex.nz = 0.6f;
    }
    const auto original_mesh = fixture.lens.mesh;
    fixture.lens.mesh = fixture.meshes.add(tilted_mesh, "tilted-glass-normals");
    const auto tilted = fixture.render({});
    // Constant blue isolates the Fresnel normal response; red/blue ratio isolates sky direction.
    CHECK(channel(tilted, 32, 32, 2) > channel(sky_on, 32, 32, 2) * 1.1f);
    const float sky_ratio = channel(sky_on, 32, 32, 0) / channel(sky_on, 32, 32, 2);
    const float tilted_ratio = channel(tilted, 32, 32, 0) / channel(tilted, 32, 32, 2);
    CHECK(std::abs(tilted_ratio - sky_ratio) > 0.2f);
    fixture.lens.mesh = original_mesh;
    fixture.params.main_eye.x = 0.3f;
    const auto grazing = fixture.render({});
    CHECK(channel(grazing, 32, 32, 2) > channel(sky_on, 32, 32, 2) * 2.0f);
    fixture.params.reflection_strength = 0.0f;
    const auto blackout = fixture.render({1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(channel(blackout, 32, 32, 0) == 0.0f);
    fixture.params.main_eye.x = 0.0f;
    const auto aligned = fixture.render({1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(channel(aligned, 32, 32, 0) > 0.8f);
}

TEST_CASE("scope refuses missing input and nonfinite optical parameters without changing HDR") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan required for scope lens proof");
        return;
    }
    LensFixture fixture(*device);
    const auto missing = fixture.render({1.0f, 1.0f, 1.0f, 1.0f}, false, false);
    CHECK(channel(missing, 32, 32, 0) == 0.125f);
    fixture.params.eye_box_radius = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = fixture.render({1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(missing.rgb == invalid.rgb);
}

TEST_CASE("scope virtual image projection responds to optical camera and eye displacement") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan required for scope lens proof");
        return;
    }
    LensFixture fixture(*device);
    rhi::TextureDesc input{};
    input.extent = {kSize, kSize};
    input.format = rhi::Format::RGBA8Unorm;
    input.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    fixture.optical_input = device->create_texture(input);
    std::array<std::uint8_t, kSize * kSize * 4> pixels{};
    for (unsigned y = 0; y < kSize; ++y)
        for (unsigned x = 0; x < kSize; ++x) {
            pixels[(y * kSize + x) * 4] = static_cast<std::uint8_t>(x * 4);
            pixels[(y * kSize + x) * 4 + 3] = 255;
        }
    device->write_texture(fixture.optical_input, pixels.data(), pixels.size());
    fixture.params.reflection_strength = 0.0f;
    fixture.params.focus_distance = 1.0f;
    fixture.params.optical_tan_half_fov = 0.5f;
    fixture.params.eye_box_radius = 0.5f;
    const auto projection = core::perspective(2.0f * std::atan(0.5f), 1.0f, 0.01f, 1000.0f);
    fixture.params.optical_view_proj = projection * fixture.params.optic_from_world;
    const auto aligned = fixture.render({});
    fixture.params.main_eye.x = 0.08f;
    const auto displaced = fixture.render({});
    CHECK(channel(aligned, 32, 32, 0) > channel(displaced, 32, 32, 0) + 0.04f);
    fixture.params.main_eye.x = 0.0f;
    fixture.params.optical_view_proj =
        projection * core::mat4_translation({-0.15f, 0.0f, 0.0f}) * fixture.params.optic_from_world;
    const auto camera_shifted = fixture.render({});
    CHECK(channel(aligned, 32, 32, 0) > channel(camera_shifted, 32, 32, 0) + 0.1f);
    // All three perturbations still leave the parent pixels outside the physical lens identical.
    CHECK(channel(aligned, 5, 32, 0) == channel(displaced, 5, 32, 0));
    CHECK(channel(aligned, 5, 32, 0) == channel(camera_shifted, 5, 32, 0));
}

TEST_CASE("physical scope at ninety millimetres needs a parent near plane inside the ocular") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan required for scope lens proof");
        return;
    }
    LensFixture fixture(*device);
    // Match the play asset's convention: the disc is offset INSIDE the weapon mesh, and both
    // weapon and optic follow a translated/rotated world camera. Identity-only probes cannot
    // reveal a camera-space ocular clipped before its fragment shader ever executes.
    constexpr core::Vec3 ocular{0.14f, -0.118f, -0.085f};
    auto mesh = lens_disc();
    for (auto& vertex : mesh.vertices) {
        vertex.px = ocular.x + vertex.px * 0.052f;
        vertex.py = ocular.y + vertex.py * 0.052f;
        vertex.pz = ocular.z;
    }
    fixture.lens.mesh = fixture.meshes.add(mesh, "offset-physical-ocular");
    core::Transform camera{};
    camera.translation = {7.0f, 2.0f, -11.0f};
    camera.rotation = core::quat_from_euler(0.17f, 1.2f, 0.0f);
    const auto camera_world = core::to_matrix(camera);
    fixture.lens.model = camera_world * core::mat4_translation({-0.14f, 0.118f, -0.005f});
    const auto optic_world = fixture.lens.model * core::mat4_translation(ocular);
    fixture.params.optic_from_world = core::inverse(optic_world);
    fixture.params.main_eye = camera.translation;
    fixture.params.lens_radius = 0.026f;
    fixture.params.optical_view_proj =
        core::perspective(2.0f * std::atan(0.04f), 1.0f, 0.01f, 1000.0f) *
        fixture.params.optic_from_world;
    const auto view = core::inverse(camera_world);
    fixture.params.main_view_proj = core::perspective(1.1f, 1.0f, 0.1f, 1000.0f) * view;
    const auto clipped = fixture.render({1.0f, 0.0f, 0.0f, 1.0f});
    fixture.params.main_view_proj = core::perspective(1.1f, 1.0f, 0.02f, 1000.0f) * view;
    const auto visible = fixture.render({1.0f, 0.0f, 0.0f, 1.0f});
    // The near-plane sensitivity control must produce a strong lens signal. Black glass alone
    // cannot distinguish failed optical sampling from a lens with zero rasterized fragments.
    CHECK(channel(clipped, 32, 32, 0) == 0.125f);
    CHECK(channel(visible, 32, 32, 0) > 0.8f);
    unsigned affected = 0;
    for (unsigned y = 0; y < kSize; ++y)
        for (unsigned x = 0; x < kSize; ++x)
            if (std::abs(channel(visible, x, y, 0) - channel(clipped, x, y, 0)) > 0.1f)
                ++affected;
    CHECK(affected > 300);
    CHECK(channel(clipped, 2, 32, 0) == channel(visible, 2, 32, 0));
}
