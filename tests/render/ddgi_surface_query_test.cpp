// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
// Exercise the production forward shader with fixed, real-format atlases. This pins a stable
// surface-query policy; it does NOT establish that probe-plane noise caused the city video.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <vector>

#include "render_test_support.hpp"
#include "rime/render/lighting/clustered.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/lighting/local_shadows.hpp"
#include "rime/render/lighting/shadows.hpp"
#include "rime/render/lighting/sky.hpp"

using namespace rime;
using namespace rime::render;

namespace {
constexpr std::uint32_t kSize = 32, kDims = 8, kPerRow = kDims * kDims;
constexpr std::array<float, 3> kBright{1.0f, 0.5f, 0.25f};
constexpr std::array<float, 3> kFallback{0.125f, 0.125f, 0.125f};
enum class Field { SurfacePlane, CoveredBlack, Occluded };

struct Fixture {
    rhi::Device& device;
    ForwardPbrPass forward;
    MeshRegistry meshes;
    CascadedShadowMap csm;
    LocalShadowMap local;
    ClusteredLights clusters;
    SkyPass sky;
    rhi::SamplerHandle linear;
    rhi::TextureHandle white, dummy;
    std::array<MeshId, 2> planes;
    std::vector<rhi::TextureHandle> textures;
    std::vector<rhi::BufferHandle> buffers;

    explicit Fixture(rhi::Device& d)
        : device(d), forward(d), meshes(d), csm(d), local(d), clusters(d), sky(d) {
        rhi::SamplerDesc sampler{};
        sampler.mag_filter = sampler.min_filter = rhi::Filter::Linear;
        sampler.address_mode = rhi::AddressMode::ClampToEdge;
        linear = device.create_sampler(sampler);
        const std::array<float, 4> pixel{1, 1, 1, 1};
        white = upload({1, 1}, rhi::Format::RGBA32Float, pixel.data(), sizeof(pixel));
        rhi::TextureDesc depth{};
        depth.extent = {1, 1};
        depth.array_layers = 2;
        depth.format = kDepthFormat;
        depth.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        dummy = device.create_texture(depth);
        textures.push_back(dummy);
        auto cmd = device.begin_commands();
        cmd->texture_barrier(dummy, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderRead);
        device.submit_blocking(*cmd);
        auto plane = make_plane(100);
        planes[0] = meshes.add(plane, "ddgi-query-floor");
        for (auto& vertex : plane.vertices)
            vertex.ny = -1;
        planes[1] = meshes.add(plane, "ddgi-query-ceiling");
    }

    ~Fixture() {
        for (auto t : textures)
            device.destroy(t);
        for (auto b : buffers)
            device.destroy(b);
        device.destroy(linear);
    }

    rhi::TextureHandle
    upload(rhi::Extent2D extent, rhi::Format format, const void* bytes, std::size_t size) {
        rhi::TextureDesc desc{};
        desc.extent = extent;
        desc.format = format;
        desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        auto texture = device.create_texture(desc);
        device.write_texture(texture, bytes, size);
        textures.push_back(texture);
        return texture;
    }

    rhi::BufferHandle uniform(const void* data, std::size_t size) {
        rhi::BufferDesc desc{};
        desc.size = size;
        desc.usage = rhi::BufferUsage::Uniform;
        desc.memory = rhi::MemoryUsage::CpuToGpu;
        auto buffer = device.create_buffer(desc);
        device.write_buffer(buffer, data, size);
        buffers.push_back(buffer);
        return buffer;
    }

    std::array<float, 3> run(float height = 0,
                             float yaw = 0,
                             float normal = 1,
                             Field field = Field::SurfacePlane,
                             bool finite_support = true) {
        const core::Vec3 eye{-10, normal * 1.5f, 2}, point{-10, height, -1.25f};
        const float pitch = -normal * 0.624f;
        const core::Vec3 direction{
            -std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
        const auto view = core::look_at(eye, eye + direction, {0, 1, 0});
        auto projection = core::perspective(1.0f, 1.0f, 0.1f, 100.0f);
        const auto q = view * core::Vec4{point.x, point.y, point.z, 1};
        constexpr float center = (16.5f / kSize) * 2 - 1;
        // An off-axis projection pins this EXACT world-plane point at pixel (16,16), despite
        // real yaw. Comparing arbitrary same-screen pixels would compare different floor points.
        projection.at(0, 2) = projection.at(0, 0) * q.x / -q.z - center;
        projection.at(1, 2) = projection.at(1, 1) * q.y / -q.z - center;
        GpuFrameUniforms frame{};
        frame.view_proj = projection * view;
        frame.view_proj_unjittered = frame.prev_view_proj = frame.view_proj;
        frame.camera_pos[0] = eye.x;
        frame.camera_pos[1] = eye.y;
        frame.camera_pos[2] = eye.z;
        for (unsigned i = 0; i < 3; ++i)
            frame.ambient[i] = kFallback[i];
        const auto clip = frame.view_proj * core::Vec4{point.x, point.y, point.z, 1};
        REQUIRE(std::fabs(clip.x / clip.w - center) < 1e-5f);
        REQUIRE(std::fabs(clip.y / clip.w - center) < 1e-5f);
        GpuDrawUniforms draw{};
        draw.model = core::mat4_translation({0, height, 0});
        draw.normal_matrix = core::Mat4{};
        for (float& channel : draw.base_color)
            channel = 1;
        draw.params[1] = 1; // dielectric, fully rough, flat normal, no texture AO
        draw.params[2] = draw.params[3] = 0;
        DrawItem item{};
        item.mesh = planes[normal < 0 ? 1 : 0];
        item.index_count = 6;
        item.flags = DrawItem::DoubleSided;
        SceneDrawData data{};
        data.meshes = &meshes;
        data.draws = std::span(&item, 1);
        data.bind_frame_ubo(uniform(&frame, sizeof(frame)), 0, sizeof(frame));
        data.draw_ubo = uniform(&draw, sizeof(draw));
        data.material_sampler = data.clamp_sampler = linear;
        data.base_color_textures = data.metallic_roughness_textures = data.normal_textures =
            data.occlusion_textures = data.emissive_textures = std::span(&white, 1);

        const rhi::Extent2D ie{kPerRow * kDdgiIrradianceTileSize, kDims * kDdgiIrradianceTileSize};
        const std::array<std::uint16_t, 4> color{
            field == Field::CoveredBlack ? std::uint16_t{0} : std::uint16_t{0x3c00},
            field == Field::CoveredBlack ? std::uint16_t{0} : std::uint16_t{0x3800},
            field == Field::CoveredBlack ? std::uint16_t{0} : std::uint16_t{0x3400},
            0x3c00};
        std::vector<std::uint16_t> irradiance(static_cast<std::size_t>(ie.width) * ie.height * 4);
        for (std::size_t i = 0; i < irradiance.size(); ++i)
            irradiance[i] = color[i % 4];
        const auto ir =
            upload(ie, rhi::Format::RGBA16Float, irradiance.data(), irradiance.size() * 2);
        const rhi::Extent2D ve{kPerRow * kDdgiVisibilityTileSize, kDims * kDdgiVisibilityTileSize};
        std::vector<float> visibility(static_cast<std::size_t>(ve.width) * ve.height * 2);
        for (std::uint32_t y = 0; y < ve.height; ++y)
            for (std::uint32_t x = 0; x < ve.width; ++x) {
                const auto global =
                    x / kDdgiVisibilityTileSize + (y / kDdgiVisibilityTileSize) * kPerRow;
                const auto probe_y = (global / kDims) % kDims;
                const bool open =
                    field == Field::CoveredBlack ||
                    (field == Field::SurfacePlane && (normal > 0 ? probe_y > 4 : probe_y < 4));
                const float mean = open ? 100.0f : 0.0f;
                const auto offset = 2 * (static_cast<std::size_t>(y) * ve.width + x);
                visibility[offset] = mean;
                visibility[offset + 1] = mean * mean;
            }
        const auto vis =
            upload(ve, rhi::Format::RG32Float, visibility.data(), visibility.size() * 4);
        GpuDdgiSampleParams params{};
        params.grid_origin_spacing[0] = -24;
        params.grid_origin_spacing[1] = -16; // y=0 is exactly probe layer four, spacing four metres
        params.grid_origin_spacing[2] = -12;
        params.grid_origin_spacing[3] = 4;
        params.grid_dims_perrow[0] = params.grid_dims_perrow[1] = params.grid_dims_perrow[2] =
            kDims;
        params.grid_dims_perrow[3] = kPerRow;
        params.enabled_pad[0] = 1;
        params.enabled_pad[1] = finite_support ? 1u : 0u;
        RenderGraph graph(device);
        const auto import = [&](rhi::TextureHandle t) {
            return graph.import_texture(t, rhi::ResourceState::ShaderRead);
        };
        DdgiBinding ddgi{
            import(ir), import(vis), graph.push_frame_data(&params, sizeof(params)), linear};
        const auto hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "ddgi-surface-hdr"});
        const auto depth =
            graph.create_texture({{kSize, kSize}, kDepthFormat, "ddgi-surface-depth"});
        forward.add_shadowed(graph,
                             hdr,
                             depth,
                             false,
                             data,
                             csm.empty_binding(graph, dummy),
                             local.empty_binding(graph, dummy),
                             clusters.empty_binding(graph),
                             ddgi,
                             sky.empty_binding(graph));
        graph.export_texture(hdr);
        auto cmd = device.begin_commands();
        graph.execute(*cmd);
        device.submit_blocking(*cmd);
        const auto image = test::decode_hdr(
            test::read_texture(device, graph.physical(hdr), kSize, kSize, 8), kSize, kSize);
        const auto offset = (16 * kSize + 16) * 3;
        return {image.rgb[offset], image.rgb[offset + 1], image.rgb[offset + 2]};
    }
};

void close(const std::array<float, 3>& pixel, const std::array<float, 3>& expected) {
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(std::isfinite(pixel[i]));
        CHECK(std::fabs(pixel[i] - expected[i]) < 0.002f);
    }
}
} // namespace

TEST_CASE("ddgi surface query: production forward depth-plane floor and ceiling stay openly lit") {
    auto device = rhi::create_device({});
    if (!device) {
        REQUIRE_FALSE(test::vulkan_required());
        return;
    }
    Fixture fixture(*device);
    for (float normal : {1.0f, -1.0f})
        for (float height : {-0.001f, 0.0f, 0.001f})
            for (float yaw : {0.0f, -0.312f, 0.312f, 0.0f}) {
                CAPTURE(normal);
                CAPTURE(height);
                CAPTURE(yaw);
                close(fixture.run(height, yaw, normal), kBright);
            }
    // The legacy authored-volume mode also uses the same stable-side surface query.
    close(fixture.run(0, 0, 1, Field::SurfacePlane, false), kBright);
}

TEST_CASE("ddgi surface query: measured black and rejected probes remain distinct") {
    auto device = rhi::create_device({});
    if (!device) {
        REQUIRE_FALSE(test::vulkan_required());
        return;
    }
    Fixture fixture(*device);
    close(fixture.run(0, 0, 1, Field::CoveredBlack), {0, 0, 0});
    close(fixture.run(0, 0, 1, Field::Occluded), kFallback);
    close(fixture.run(0, 0, 1, Field::Occluded, false), {0, 0, 0});
    // Beyond the ten-centimetre query bias, the blocked side must not gain the bright atlas.
    close(fixture.run(-0.2f), kFallback);
    close(fixture.run(0.2f, 0, -1), kFallback);
    REQUIRE(std::fabs(kBright[0] - kFallback[0]) > 0.8f);
}
