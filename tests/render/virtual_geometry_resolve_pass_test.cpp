// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the M18 step-2 material resolve, on a real Vulkan device: the visibility buffer +
// resolve must reproduce what a CONVENTIONAL forward draw of the same mesh produces, pixel for
// pixel. Structural, never golden — the reference is rendered in the same test, from the same
// cooked vertex bytes, with the same matrix, cull mode and depth test.
//
// The mesh is a textured floor seen in perspective (so the perspective correction in the
// analytic barycentrics is load-bearing: with w = 1 everywhere a wrong correction would pass),
// split down x = 0 into two leaf clusters with material slots 0 and 1. Each material's albedo is
// a mip chain whose levels are distinct solid colours, sampled trilinearly, so the albedo is a
// direct readout of the LOD each path chose — which is what makes the resolve's analytic UV
// gradients observable. Asserted:
//
//   (a) the covered-pixel sets are identical, and the material ID matches exactly per pixel;
//   (b) UV matches within kUvEps on every covered pixel; the UV gradients within kGradRelEps
//       (relative) and the albedo within kAlbedoEps per channel where the reference's fine
//       derivatives are forward differences (the even column/row of each 2x2 quad);
//   (c) a pixel whose generation the cluster table does not hold resolves to the visible stale
//       marker, never to "empty"; an empty frame or a bad material list is counted and clears.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_resolve_pass.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/render/virtual_geometry_visibility_pass.hpp"
#include "rime/rhi/device.hpp"
#include "vg_forward_reference.frag.spv.h"
#include "vg_forward_reference.vert.spv.h"

namespace {

using namespace rime;
using namespace rime::render;

constexpr std::uint32_t kSize = 64;
constexpr assets::AssetId kAssetId{77};
constexpr std::uint32_t kStride = 32; // cooked v1: position, normal, uv
constexpr std::uint32_t kTexSize = 64;
constexpr std::uint32_t kTexLevels = 7; // 64 → 1

// Tolerances, each ~3-10x the worst case measured on an RTX 3060 (2026-09-23: |Δuv| 7.8e-5,
// gradient 9.7e-5 relative, albedo 1/255). UV is interpolated by two different float pipelines
// (hardware perspective-correct interpolation vs. the analytic reconstruction), and the error
// grows toward the horizon where v changes fastest per pixel. Gradients and albedo are compared
// only where the reference's fine derivatives share the resolve's definition (see the loop).
constexpr float kUvEps = 2.5e-4f;
constexpr float kGradRelEps = 1e-3f;
constexpr int kAlbedoEps = 3;

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

static_assert(sizeof(Vertex) == kStride);

// The floor, x ∈ [-1.5, 1.5], z ∈ [1, -6] at y = 0; u = (x + 1.5) / 3, v = (1 - z) / 7. Each
// cluster is one half-quad, emitted in BOTH windings so the proof does not depend on the front
// face convention (the back-facing copy is culled identically by both paths).
std::array<Vertex, 4> half_quad(float x0, float x1) {
    const auto v = [](float x, float z) {
        return Vertex{{x, 0.0f, z}, {0.0f, 1.0f, 0.0f}, {(x + 1.5f) / 3.0f, (1.0f - z) / 7.0f}};
    };
    return {v(x0, 1.0f), v(x1, 1.0f), v(x1, -6.0f), v(x0, -6.0f)};
}

constexpr std::uint32_t kClusterIndices[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};

// One page: both clusters' vertices, then both clusters' cluster-local indices. Cluster 0 is the
// permanent coarse root (the whole floor, unused as a draw); clusters 1 and 2 are the leaves.
assets::VirtualGeometryAsset floor_asset() {
    assets::VirtualGeometryAsset asset{};
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = kStride;
    const auto append = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        asset.page_bytes.insert(asset.page_bytes.end(), bytes, bytes + size);
    };
    const std::array<Vertex, 4> root = half_quad(-1.5f, 1.5f);
    append(root.data(), sizeof(root));
    append(kClusterIndices, sizeof(kClusterIndices));
    const auto page0 = static_cast<std::uint32_t>(asset.page_bytes.size());
    const std::array<Vertex, 4> left = half_quad(-1.5f, 0.0f);
    const std::array<Vertex, 4> right = half_quad(0.0f, 1.5f);
    append(left.data(), sizeof(left));
    append(right.data(), sizeof(right));
    append(kClusterIndices, sizeof(kClusterIndices));
    append(kClusterIndices, sizeof(kClusterIndices));
    const auto page1 = static_cast<std::uint32_t>(asset.page_bytes.size()) - page0;

    asset.pages = {{0, page0, 0, 1, 0, 0, true}, {page0, page1, 1, 2, 0, 0, false}};
    const std::array<std::array<std::uint32_t, 5>, 3> layout = {{
        // page, vertex_offset, first_index, group, material
        {0, 0, 0, 0, 0},
        {1, 0, 0, 1, 0},
        {1, 4 * kStride, 12, 1, 1},
    }};
    for (const auto& l : layout) {
        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {-1.5f, 0.0f, -6.0f};
        cluster.bounds.max = {1.5f, 0.0f, 1.0f};
        cluster.page = l[0];
        cluster.vertex_offset = l[1];
        cluster.first_index = l[2];
        cluster.vertex_count = 4;
        cluster.index_count = 12;
        cluster.replacement_group = l[3];
        cluster.material_slot = l[4];
        asset.clusters.push_back(cluster);
    }
    asset.groups = {{0, 1, 0, 1, 2.0f, true}, {1, 2, 0, 0, 0.1f, false}};
    asset.child_groups = {1};
    asset.coarse_group = 0;
    return asset;
}

core::Mat4 floor_mvp() {
    return core::perspective(std::numbers::pi_v<float> / 3.0f, 1.0f, 0.1f, 100.0f) *
           core::look_at({0.0f, 1.0f, 2.0f}, {0.0f, 0.0f, -2.0f}, {0.0f, 1.0f, 0.0f});
}

// A mip chain of distinct solid colours; the two materials use different ramps.
rhi::TextureHandle make_albedo(rhi::Device& device, std::uint32_t material) {
    std::vector<std::vector<std::byte>> levels;
    std::vector<rhi::MipData> mips;
    for (std::uint32_t level = 0, size = kTexSize; level < kTexLevels; ++level, size /= 2) {
        const auto ramp = static_cast<std::uint8_t>(level * 36);
        const std::array<std::uint8_t, 4> rgba =
            material == 0
                ? std::array<std::uint8_t, 4>{ramp, static_cast<std::uint8_t>(255 - ramp), 128, 255}
                : std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(255 - ramp), 64, ramp, 255};
        std::vector<std::byte> pixels(std::size_t{size} * size * 4);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::byte>(rgba[i % 4]);
        }
        levels.push_back(std::move(pixels));
    }
    for (const auto& l : levels) {
        mips.push_back({std::span<const std::byte>(l)});
    }
    rhi::TextureDesc td{};
    td.extent = {kTexSize, kTexSize};
    td.mip_levels = kTexLevels;
    td.format = rhi::Format::RGBA8Unorm;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "vg-resolve-albedo";
    const rhi::TextureHandle tex = device.create_texture(td);
    device.write_texture_mips(tex, mips);
    return tex;
}

struct Frame {
    std::vector<std::uint32_t> material;
    std::vector<std::array<float, 4>> uv;
    std::vector<std::array<std::uint8_t, 4>> albedo;
};

struct Targets {
    RGTexture material;
    RGTexture uv;
    RGTexture albedo;
};

Targets make_targets(RenderGraph& graph) {
    Targets t{graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-material"}),
              graph.create_texture({{kSize, kSize}, rhi::Format::RGBA32Float, "vg-uv"}),
              graph.create_texture({{kSize, kSize}, rhi::Format::RGBA8Unorm, "vg-albedo"})};
    graph.export_texture(t.material);
    graph.export_texture(t.uv);
    graph.export_texture(t.albedo);
    return t;
}

// Execute `graph`, copy the three targets back, and decode them.
Frame read_back(rhi::Device& device, RenderGraph& graph, const Targets& t) {
    const std::size_t n = std::size_t{kSize} * kSize;
    const auto buffer = [&](std::size_t bytes) {
        rhi::BufferDesc bd{};
        bd.size = bytes;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryUsage::GpuToCpu;
        return device.create_buffer(bd);
    };
    const rhi::BufferHandle mb = buffer(n * 4);
    const rhi::BufferHandle ub = buffer(n * 16);
    const rhi::BufferHandle ab = buffer(n * 4);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    cmd->copy_texture_to_buffer(graph.physical(t.material), mb);
    cmd->copy_texture_to_buffer(graph.physical(t.uv), ub);
    cmd->copy_texture_to_buffer(graph.physical(t.albedo), ab);
    device.submit_blocking(*cmd);

    Frame f;
    f.material.resize(n);
    f.uv.resize(n);
    f.albedo.resize(n);
    device.read_buffer(mb, f.material.data(), n * 4);
    device.read_buffer(ub, f.uv.data(), n * 16);
    device.read_buffer(ab, f.albedo.data(), n * 4);
    device.destroy(mb);
    device.destroy(ub);
    device.destroy(ab);
    return f;
}

// The conventional path: vertex attributes + index buffer + hardware interpolation.
class ForwardReference {
public:
    explicit ForwardReference(rhi::Device& device) : device_(device) {
        rhi::ShaderDesc vs{};
        vs.stage = rhi::ShaderStage::Vertex;
        vs.spirv = vg_forward_reference_vert_spv;
        vs.spirv_size_bytes = sizeof(vg_forward_reference_vert_spv);
        vs_ = device.create_shader(vs);
        rhi::ShaderDesc fs{};
        fs.stage = rhi::ShaderStage::Fragment;
        fs.spirv = vg_forward_reference_frag_spv;
        fs.spirv_size_bytes = sizeof(vg_forward_reference_frag_spv);
        fs_ = device.create_shader(fs);

        static constexpr rhi::VertexAttribute kAttrs[] = {{0, rhi::Format::RGB32Float, 0},
                                                          {1, rhi::Format::RG32Float, 24}};
        static constexpr rhi::Format kFormats[] = {
            rhi::Format::R32Uint, rhi::Format::RGBA32Float, rhi::Format::RGBA8Unorm};
        static constexpr rhi::BindingDesc kBindings[] = {
            {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}};
        rhi::GraphicsPipelineDesc pd{};
        pd.vertex_shader = vs_;
        pd.fragment_shader = fs_;
        pd.vertex_layout.stride = kStride;
        pd.vertex_layout.attributes = kAttrs;
        pd.color_formats = kFormats;
        pd.cull = rhi::CullMode::Back; // the visibility pass's state, exactly
        pd.depth_test = true;
        pd.depth_write = true;
        pd.depth_compare = rhi::CompareOp::Less;
        pd.depth_format = kDepthFormat;
        pd.bindings = kBindings;
        pd.push_constant_size = 68;
        pipeline_ = device.create_graphics_pipeline(pd);
    }

    ~ForwardReference() {
        device_.destroy(pipeline_);
        device_.destroy(fs_);
        device_.destroy(vs_);
    }

    ForwardReference(const ForwardReference&) = delete;
    ForwardReference& operator=(const ForwardReference&) = delete;

    rhi::PipelineHandle pipeline() const { return pipeline_; }

private:
    rhi::Device& device_;
    rhi::ShaderHandle vs_;
    rhi::ShaderHandle fs_;
    rhi::PipelineHandle pipeline_;
};

struct ForwardPush {
    float mvp[16];
    std::uint32_t material;
};

static_assert(sizeof(ForwardPush) == 68);

std::size_t covered(const Frame& f) {
    return static_cast<std::size_t>(
        std::count_if(f.material.begin(), f.material.end(), [](std::uint32_t m) { return m; }));
}

} // namespace

TEST_CASE("vg resolve: visibility buffer + resolve matches a forward draw (M18.2)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping resolve proofs");
        return;
    }

    const assets::VirtualGeometryAsset asset = floor_asset();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);
    VirtualGeometryResidency residency;
    REQUIRE(residency.register_asset(kAssetId, asset));
    REQUIRE(residency.request_page(kAssetId, 1));
    REQUIRE(residency.complete_page(kAssetId, 1));
    const VirtualGeometrySelection selection{{1}, 0};
    const core::Mat4 mvp = floor_mvp();

    const std::array<VirtualGeometryClusterDraw, 2> draws = {{{1, 5, 3}, {2, 9, 3}}};
    VirtualGeometryVisibilityRequest vis_request{};
    vis_request.asset = &asset;
    vis_request.asset_id = kAssetId;
    vis_request.residency = &residency;
    vis_request.selection = &selection;
    vis_request.clusters = draws;
    vis_request.clip_from_object = mvp;

    const std::array<rhi::TextureHandle, 2> albedo = {make_albedo(*device, 0),
                                                      make_albedo(*device, 1)};
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Linear;
    sd.min_filter = rhi::Filter::Linear;
    sd.mip_filter = rhi::Filter::Linear; // trilinear: albedo is a continuous readout of the LOD
    const rhi::SamplerHandle sampler = device->create_sampler(sd);
    const std::array<VirtualGeometryResolveMaterial, 2> materials = {
        {{albedo[0], sampler}, {albedo[1], sampler}}};

    VirtualGeometryVisibilityPass visibility(*device);
    VirtualGeometryResolvePass resolve(*device);

    // One graph: visibility → resolve.
    const auto render_resolve = [&](const VirtualGeometryResolveRequest& request,
                                    VirtualGeometryVisibilityRequest vis) {
        RenderGraph graph(*device);
        const RGTexture ids =
            graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-ids"});
        const RGTexture depth_bits =
            graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-depth-bits"});
        const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "vg-depth"});
        const Targets t = make_targets(graph);
        (void)visibility.declare(graph, ids, depth_bits, depth, vis);
        VirtualGeometryResolveRequest r = request;
        if (r.clusters == nullptr) {
            r.clusters = &visibility.cluster_buffers();
        }
        (void)resolve.declare(graph, ids, {kSize, kSize}, t.material, t.uv, t.albedo, r);
        return read_back(*device, graph, t);
    };

    VirtualGeometryResolveRequest request{};
    request.clip_from_object = mvp;
    request.materials = materials;

    SUBCASE("per-pixel parity with the conventional forward draw") {
        const Frame vb = render_resolve(request, vis_request);
        CHECK(visibility.stats().drawn == 2);
        CHECK(resolve.stats().resolved == 1);

        // The reference: the same page's vertices and indices as ordinary vertex/index buffers.
        ForwardReference forward(*device);
        assets::VirtualGeometryPageView view{};
        REQUIRE(assets::view_virtual_geometry_page(asset, 1, view) ==
                assets::VirtualGeometryPageViewError::None);
        rhi::BufferDesc vbd{};
        vbd.size = 8 * kStride;
        vbd.usage = rhi::BufferUsage::Vertex;
        vbd.memory = rhi::MemoryUsage::CpuToGpu;
        vbd.initial_data = view.vertices.data(); // both leaf clusters' vertices, page order
        const rhi::BufferHandle vertex_buffer = device->create_buffer(vbd);
        rhi::BufferDesc ibd{};
        ibd.size = sizeof(kClusterIndices);
        ibd.usage = rhi::BufferUsage::Index;
        ibd.memory = rhi::MemoryUsage::CpuToGpu;
        ibd.initial_data = kClusterIndices;
        const rhi::BufferHandle index_buffer = device->create_buffer(ibd);

        RenderGraph graph(*device);
        const Targets t = make_targets(graph);
        const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "fw-depth"});
        const RGColorAttachment colors[] = {
            {t.material, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}},
            {t.uv, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}},
            {t.albedo, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
        const RGDepthAttachment depth_att{
            depth, rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f, 0, false};
        RenderGraph::RasterPassDesc desc{};
        desc.colors = colors;
        desc.depth = &depth_att;
        graph.add_raster_pass("forward-reference", desc, [&](rhi::CommandBuffer& cmd) {
            cmd.bind_pipeline(forward.pipeline());
            cmd.bind_vertex_buffer(vertex_buffer);
            cmd.bind_index_buffer(index_buffer, rhi::IndexType::Uint32);
            for (std::uint32_t c = 0; c < 2; ++c) {
                ForwardPush push{};
                std::memcpy(push.mvp, mvp.m, sizeof(push.mvp));
                push.material = asset.clusters[1 + c].material_slot + 1;
                cmd.bind_texture(0, albedo[asset.clusters[1 + c].material_slot], sampler);
                cmd.push_constants(&push, sizeof(push));
                cmd.draw_indexed(12, 1, 0, static_cast<std::int32_t>(4 * c));
            }
        });
        const Frame fw = read_back(*device, graph, t);
        device->destroy(vertex_buffer);
        device->destroy(index_buffer);

        // The scene must actually exercise the proof: both materials visible in quantity.
        const std::size_t fw_covered = covered(fw);
        const auto count_material = [](const Frame& f, std::uint32_t m) {
            return std::count(f.material.begin(), f.material.end(), m);
        };
        CHECK(fw_covered > 1000u);
        CHECK(count_material(fw, 1) > 400);
        CHECK(count_material(fw, 2) > 400);
        CHECK(count_material(vb, kVirtualGeometryResolveStale) == 0);

        std::size_t material_mismatch = 0;
        std::size_t uv_mismatch = 0;
        std::size_t grad_mismatch = 0;
        std::size_t albedo_mismatch = 0;
        float max_uv = 0.0f;
        float max_grad_rel = 0.0f;
        int max_albedo = 0;
        float min_lod_grad = 1e9f;
        float max_lod_grad = 0.0f;
        for (std::size_t i = 0; i < fw.material.size(); ++i) {
            if (fw.material[i] != vb.material[i]) {
                ++material_mismatch; // covers "covered-pixel sets identical" too (0 = empty)
                continue;
            }
            if (fw.material[i] == 0) {
                continue;
            }
            const float du = std::fabs(fw.uv[i][0] - vb.uv[i][0]);
            const float dv = std::fabs(fw.uv[i][1] - vb.uv[i][1]);
            max_uv = std::max({max_uv, du, dv});
            uv_mismatch += (du > kUvEps || dv > kUvEps) ? 1 : 0;
            // dFdxFine/dFdyFine are forward differences (to the right / below) only on the even
            // column / row of each 2x2 quad; on the odd one they difference backwards. The
            // resolve always differences forwards, so the gradients are the same DEFINITION —
            // and must agree tightly — exactly on the even columns (du/dx) and rows (dv/dy).
            const std::size_t x = i % kSize;
            const std::size_t y = i / kSize;
            for (int g = 2; g < 4; ++g) {
                if ((g == 2 ? x : y) % 2 != 0) {
                    continue;
                }
                const float ref = std::fabs(fw.uv[i][g]);
                const float rel = std::fabs(fw.uv[i][g] - vb.uv[i][g]) / std::max(ref, 1e-6f);
                max_grad_rel = std::max(max_grad_rel, rel);
                grad_mismatch += rel > kGradRelEps ? 1 : 0;
            }
            min_lod_grad = std::min(min_lod_grad, std::fabs(vb.uv[i][3]));
            max_lod_grad = std::max(max_lod_grad, std::fabs(vb.uv[i][3]));
            if (x % 2 != 0 || y % 2 != 0) {
                continue; // the albedo's LOD uses both gradients: compare where both agree
            }
            int worst = 0;
            for (int ch = 0; ch < 4; ++ch) {
                worst = std::max(worst, std::abs(int{fw.albedo[i][ch]} - int{vb.albedo[i][ch]}));
            }
            max_albedo = std::max(max_albedo, worst);
            albedo_mismatch += worst > kAlbedoEps ? 1 : 0;
        }
        MESSAGE("covered=" << fw_covered << " max|Δuv|=" << max_uv
                           << " max grad rel=" << max_grad_rel << " max|Δalbedo|=" << max_albedo
                           << " |dv/dy| range=[" << min_lod_grad << ", " << max_lod_grad << "]");
        CHECK(covered(vb) == fw_covered);
        CHECK(material_mismatch == 0);
        CHECK(uv_mismatch == 0);
        CHECK(grad_mismatch == 0);
        CHECK(albedo_mismatch == 0);
        // The LOD must span more than one mip level (texels per pixel = 64·|dv/dy|), or the
        // albedo comparison could not see a wrong gradient.
        CHECK(max_lod_grad * kTexSize > 4.0f * std::max(min_lod_grad * kTexSize, 0.5f));
    }

    SUBCASE("a pixel whose generation the table does not hold is stale, not empty") {
        // Hand the resolve a table claiming slots 5 and 9 hold generation 4; the pixels say 3.
        (void)render_resolve(request, vis_request); // populate the real buffers once
        const VirtualGeometryClusterBuffers real = visibility.cluster_buffers();
        std::vector<VirtualGeometryGpuCluster> table(real.cluster_count);
        for (const auto& d : draws) {
            table[d.cluster_slot] = {0, 0, 4, 0, 4, 1, {}};
        }
        rhi::BufferDesc bd{};
        bd.size = table.size() * sizeof(VirtualGeometryGpuCluster);
        bd.usage = rhi::BufferUsage::Storage;
        bd.memory = rhi::MemoryUsage::CpuToGpu;
        bd.initial_data = table.data();
        const rhi::BufferHandle stale_table = device->create_buffer(bd);

        // render_resolve re-declares the visibility pass (new buffers); swap only the table.
        RenderGraph graph(*device);
        const RGTexture ids =
            graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-ids"});
        const RGTexture depth_bits =
            graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-depth-bits"});
        const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "vg-depth"});
        const Targets t = make_targets(graph);
        REQUIRE(visibility.declare(graph, ids, depth_bits, depth, vis_request));
        VirtualGeometryClusterBuffers swapped = visibility.cluster_buffers();
        swapped.clusters = stale_table;
        VirtualGeometryResolveRequest r = request;
        r.clusters = &swapped;
        REQUIRE(resolve.declare(graph, ids, {kSize, kSize}, t.material, t.uv, t.albedo, r));
        const Frame f = read_back(*device, graph, t);
        device->destroy(stale_table);

        const auto stale =
            std::count(f.material.begin(), f.material.end(), kVirtualGeometryResolveStale);
        CHECK(stale > 1000);
        CHECK(static_cast<std::size_t>(stale) == covered(f)); // every covered pixel, no other
    }

    SUBCASE("an empty visibility frame resolves nothing and is counted") {
        const std::array<VirtualGeometryClusterDraw, 1> none = {{{99, 1, 0}}};
        VirtualGeometryVisibilityRequest vis = vis_request;
        vis.clusters = none;
        const Frame f = render_resolve(request, vis);
        CHECK(covered(f) == 0);
        CHECK(resolve.stats().skipped_no_clusters == 1);
        CHECK(resolve.stats().resolved == 0);
    }

    SUBCASE("a bad material list resolves nothing and is counted") {
        VirtualGeometryResolveRequest r = request;
        r.materials = {};
        const Frame f = render_resolve(r, vis_request);
        CHECK(covered(f) == 0);
        CHECK(resolve.stats().skipped_bad_materials == 1);
    }

    device->destroy(sampler);
    device->destroy(albedo[0]);
    device->destroy(albedo[1]);
}
