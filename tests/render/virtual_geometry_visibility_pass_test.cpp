// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the M18 step-1 visibility pass, on a real Vulkan device. Structural, never golden:
// the leaf cluster is a quad authored directly in clip space (the MVP is identity), covering NDC
// [-0.5, 0.5]² at z = 0.5 on a 64² target — i.e. pixels [16, 48). Every sampled pixel sits at
// least 8 px from that edge, so rasterization edge rules cannot move an assertion. The quad is
// emitted in BOTH windings so the proof does not depend on the pipeline's front-face convention.
//
//   (a) a covered pixel reads the packed visibility ID and depth bits of exactly 0.5f;
//   (b) uncovered pixels stay at the clear (the ABI's invalid ID 0, depth bits 0);
//   (c) an invalid, unselected, non-leaf or non-resident request draws nothing, leaves both
//       targets clear, and lands in exactly one skip counter.

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "rime/render/virtual_geometry_visibility_pass.hpp"
#include "rime/rhi/device.hpp"

namespace {

using namespace rime;
using namespace rime::render;

constexpr std::uint32_t kSize = 64;
constexpr assets::AssetId kAssetId{42};

// One page holding a clip-space quad: 4 vertices at the cooked v1 stride (pos, normal, uv),
// followed by 12 little-endian u32 indices (both windings). Positions only matter to the pass.
std::vector<std::byte> quad_page() {
    constexpr std::uint32_t stride = 32;
    const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    const std::uint32_t indices[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    std::vector<std::byte> bytes(4 * stride + sizeof(indices));
    for (std::uint32_t v = 0; v < 4; ++v) {
        const float position[3] = {corners[v][0], corners[v][1], 0.5f};
        std::memcpy(bytes.data() + v * stride, position, sizeof(position));
    }
    std::memcpy(bytes.data() + 4 * stride, indices, sizeof(indices)); // test host is little-endian
    return bytes;
}

// Page 0: permanent coarse group 0. Page 1: transient leaf group 1, the only child of group 0.
assets::VirtualGeometryAsset fixture() {
    assets::VirtualGeometryAsset asset{};
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = assets::expected_vertex_stride(asset.attribs);
    const std::vector<std::byte> page = quad_page();
    const auto page_size = static_cast<std::uint32_t>(page.size());
    asset.page_bytes.insert(asset.page_bytes.end(), page.begin(), page.end());
    asset.page_bytes.insert(asset.page_bytes.end(), page.begin(), page.end());
    asset.pages = {{0, page_size, 0, 1, 0, 0, true}, {page_size, page_size, 1, 1, 0, 0, false}};
    for (std::uint32_t i = 0; i < 2; ++i) {
        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {-0.5f, -0.5f, 0.5f};
        cluster.bounds.max = {0.5f, 0.5f, 0.5f};
        cluster.page = i;
        cluster.vertex_count = 4;
        cluster.index_count = 12;
        cluster.replacement_group = i;
        asset.clusters.push_back(cluster);
    }
    asset.groups = {{0, 1, 0, 1, 2.0f, true}, {1, 1, 0, 0, 0.1f, false}};
    asset.child_groups = {1};
    asset.coarse_group = 0;
    return asset;
}

struct Readback {
    std::vector<std::uint32_t> ids;
    std::vector<std::uint32_t> depth_bits;
    bool drew = false;
};

Readback render_cluster(rhi::Device& device,
                        VirtualGeometryVisibilityPass& pass,
                        const VirtualGeometryVisibilityRequest& request) {
    RenderGraph graph(device);
    const RGTexture ids = graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-ids"});
    const RGTexture depth_bits =
        graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-depth-bits"});
    const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "vg-depth"});
    graph.export_texture(ids);
    graph.export_texture(depth_bits);

    Readback out;
    out.drew = pass.declare(graph, ids, depth_bits, depth, request);

    rhi::BufferDesc bd{};
    bd.size = kSize * kSize * sizeof(std::uint32_t);
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle id_buffer = device.create_buffer(bd);
    const rhi::BufferHandle depth_buffer = device.create_buffer(bd);

    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    cmd->copy_texture_to_buffer(graph.physical(ids), id_buffer);
    cmd->copy_texture_to_buffer(graph.physical(depth_bits), depth_buffer);
    device.submit_blocking(*cmd);

    out.ids.resize(kSize * kSize);
    out.depth_bits.resize(kSize * kSize);
    device.read_buffer(id_buffer, out.ids.data(), bd.size);
    device.read_buffer(depth_buffer, out.depth_bits.data(), bd.size);
    device.destroy(id_buffer);
    device.destroy(depth_buffer);
    return out;
}

std::uint32_t at(const std::vector<std::uint32_t>& image, std::uint32_t x, std::uint32_t y) {
    return image[y * kSize + x];
}

void check_all_clear(const Readback& r) {
    std::size_t nonzero = 0;
    for (std::size_t i = 0; i < r.ids.size(); ++i) {
        nonzero += (r.ids[i] != 0 || r.depth_bits[i] != 0) ? 1 : 0;
    }
    CHECK(nonzero == 0);
}

} // namespace

TEST_CASE("vg visibility: a resident selected leaf writes its packed id and depth (M18.1)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping visibility pass proofs");
        return;
    }

    const assets::VirtualGeometryAsset asset = fixture();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);
    VirtualGeometryResidency residency;
    REQUIRE(residency.register_asset(kAssetId, asset));
    REQUIRE(residency.request_page(kAssetId, 1));
    REQUIRE(residency.complete_page(kAssetId, 1));

    const VirtualGeometrySelection selection =
        select_virtual_geometry(asset,
                                {.pixels_per_metre = 2.0f,
                                 .max_projected_error_px = 1.0f,
                                 .page_resident = residency.page_residency_bytes(kAssetId)});
    REQUIRE(selection.groups == std::vector<std::uint32_t>{1});

    VirtualGeometryVisibilityRequest request{};
    request.asset = &asset;
    request.asset_id = kAssetId;
    request.residency = &residency;
    request.selection = &selection;
    request.cluster = 1;
    request.cluster_slot = 7;
    request.generation = 3;
    request.clip_from_object = core::identity();
    const std::uint32_t expected_id = *pack_virtual_geometry_visibility_id({7, 3, 1});

    VirtualGeometryVisibilityPass pass(*device);

    SUBCASE("covered pixels carry the id and depth; uncovered pixels stay clear") {
        const Readback r = render_cluster(*device, pass, request);
        CHECK(r.drew);
        CHECK(pass.stats().drawn == 1);
        const std::array<std::array<std::uint32_t, 2>, 5> covered = {
            {{32, 32}, {24, 24}, {40, 24}, {24, 40}, {40, 40}}};
        for (const auto& p : covered) {
            CHECK(at(r.ids, p[0], p[1]) == expected_id);
            CHECK(at(r.depth_bits, p[0], p[1]) == std::bit_cast<std::uint32_t>(0.5f));
        }
        const auto unpacked = unpack_virtual_geometry_visibility_id(at(r.ids, 32, 32));
        REQUIRE(unpacked.has_value());
        CHECK(unpacked->cluster == 7);
        CHECK(unpacked->generation == 3);
        const std::array<std::array<std::uint32_t, 2>, 5> uncovered = {
            {{2, 2}, {61, 2}, {2, 61}, {61, 61}, {32, 4}}};
        for (const auto& p : uncovered) {
            CHECK(at(r.ids, p[0], p[1]) == kInvalidVirtualGeometryVisibilityId);
            CHECK(at(r.depth_bits, p[0], p[1]) == 0u);
        }
        // Coverage is the quad and only the quad: 32×32 pixels, with no edge ambiguity because
        // pixel centres never land on x/y = 16 or 48 exactly.
        std::size_t covered_count = 0;
        for (const std::uint32_t id : r.ids) {
            covered_count += id == expected_id ? 1 : 0;
        }
        CHECK(covered_count == 32u * 32u);
    }

    SUBCASE("an invalid cluster index draws nothing and is counted") {
        request.cluster = 99;
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_invalid_request == 1);
        CHECK(pass.stats().drawn == 0);
    }

    SUBCASE("a cluster outside the selected cut draws nothing and is counted") {
        request.cluster = 0; // the coarse group was replaced by its leaf
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_not_selected == 1);
    }

    SUBCASE("a selected non-leaf group draws nothing and is counted") {
        const VirtualGeometrySelection coarse{{0}, 0};
        request.selection = &coarse;
        request.cluster = 0;
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_not_leaf == 1);
    }

    SUBCASE("a stale selection whose page was evicted draws nothing and is counted") {
        REQUIRE(residency.evict_page(kAssetId, 1));
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_not_resident == 1);
        CHECK(pass.stats().drawn == 0);
    }

    SUBCASE("an id that does not fit the ABI draws nothing and is counted") {
        request.cluster_slot = kVirtualGeometryVisibilityMaxCluster + 1;
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_bad_id == 1);
    }

    SUBCASE("a drawn frame followed by a rejected one is cleared, not stale") {
        (void)render_cluster(*device, pass, request);
        request.cluster = 99;
        const Readback r = render_cluster(*device, pass, request);
        check_all_clear(r);
        CHECK(pass.stats().drawn == 1);
        CHECK(pass.stats().skipped_invalid_request == 1);
    }
}
