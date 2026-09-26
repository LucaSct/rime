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
#include <utility>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_gpu_selection.hpp"
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
    std::vector<VirtualGeometryVisibilityWords> ids; // RG32Uint readback
    std::vector<std::uint32_t> depth_bits;
    bool drew = false;
};

Readback render_cluster(rhi::Device& device,
                        VirtualGeometryVisibilityPass& pass,
                        const VirtualGeometryVisibilityRequest& request) {
    RenderGraph graph(device);
    const RGTexture ids = graph.create_texture({{kSize, kSize}, rhi::Format::RG32Uint, "vg-ids"});
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
    const rhi::BufferHandle depth_buffer = device.create_buffer(bd);
    rhi::BufferDesc ibd = bd;
    ibd.size = kSize * kSize * sizeof(VirtualGeometryVisibilityWords); // 8 bytes: RG32Uint
    const rhi::BufferHandle id_buffer = device.create_buffer(ibd);

    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    cmd->copy_texture_to_buffer(graph.physical(ids), id_buffer);
    cmd->copy_texture_to_buffer(graph.physical(depth_bits), depth_buffer);
    device.submit_blocking(*cmd);

    out.ids.resize(kSize * kSize);
    out.depth_bits.resize(kSize * kSize);
    device.read_buffer(id_buffer, out.ids.data(), ibd.size);
    device.read_buffer(depth_buffer, out.depth_bits.data(), bd.size);
    device.destroy(id_buffer);
    device.destroy(depth_buffer);
    return out;
}

template <typename T> T at(const std::vector<T>& image, std::uint32_t x, std::uint32_t y) {
    return image[y * kSize + x];
}

// A pixel's ID with the triangle field masked off: which cluster, generation and version.
VirtualGeometryVisibilityWords cluster_bits(VirtualGeometryVisibilityWords id) {
    return {id.lo & ~kVirtualGeometryVisibilityV3MaxTriangle, id.hi};
}

void check_all_clear(const Readback& r) {
    std::size_t nonzero = 0;
    for (std::size_t i = 0; i < r.ids.size(); ++i) {
        nonzero +=
            (!(r.ids[i] == kInvalidVirtualGeometryVisibilityWords) || r.depth_bits[i] != 0) ? 1 : 0;
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
    VirtualGeometryClusterDraw draw{1, 7, 3};
    request.clusters = {&draw, 1};
    request.clip_from_object = core::identity();
    const VirtualGeometryVisibilityWords expected_id =
        *pack_virtual_geometry_visibility_id64({7, 3}); // triangle 0

    VirtualGeometryVisibilityPass pass(*device);

    SUBCASE("covered pixels carry the id and depth; uncovered pixels stay clear") {
        const Readback r = render_cluster(*device, pass, request);
        CHECK(r.drew);
        CHECK(pass.stats().drawn == 1);
        const std::array<std::array<std::uint32_t, 2>, 5> covered = {
            {{32, 32}, {24, 24}, {40, 24}, {24, 40}, {40, 40}}};
        for (const auto& p : covered) {
            CHECK(cluster_bits(at(r.ids, p[0], p[1])) == expected_id);
            CHECK(at(r.depth_bits, p[0], p[1]) == std::bit_cast<std::uint32_t>(0.5f));
        }
        const auto unpacked = unpack_virtual_geometry_visibility_id64(at(r.ids, 32, 32));
        REQUIRE(unpacked.has_value());
        CHECK(unpacked->cluster == 7);
        CHECK(unpacked->generation == 3);
        CHECK(unpacked->version == kVirtualGeometryVisibilityCurrentVersion);
        const std::array<std::array<std::uint32_t, 2>, 5> uncovered = {
            {{2, 2}, {61, 2}, {2, 61}, {61, 61}, {32, 4}}};
        for (const auto& p : uncovered) {
            CHECK(at(r.ids, p[0], p[1]) == kInvalidVirtualGeometryVisibilityWords);
            CHECK(at(r.depth_bits, p[0], p[1]) == 0u);
        }
        // Coverage is the quad and only the quad: 32×32 pixels, with no edge ambiguity because
        // pixel centres never land on x/y = 16 or 48 exactly.
        std::size_t covered_count = 0;
        for (const VirtualGeometryVisibilityWords id : r.ids) {
            covered_count += cluster_bits(id) == expected_id ? 1 : 0;
        }
        CHECK(covered_count == 32u * 32u);
        // The triangle field is real: one winding's two triangles (0,1 or the reversed copies
        // 2,3 — which depends on the front-face convention) each own half, the other is culled.
        std::array<std::size_t, 4> per_triangle{};
        for (const VirtualGeometryVisibilityWords id : r.ids) {
            if (!(id == kInvalidVirtualGeometryVisibilityWords)) {
                ++per_triangle[unpack_virtual_geometry_visibility_id64(id)->triangle & 3u];
            }
        }
        CHECK(per_triangle[0] + per_triangle[1] + per_triangle[2] + per_triangle[3] == 32u * 32u);
        const bool first_winding = per_triangle[0] != 0;
        CHECK(per_triangle[first_winding ? 0 : 2] > 400u);
        CHECK(per_triangle[first_winding ? 1 : 3] > 400u);
        CHECK(per_triangle[first_winding ? 2 : 0] + per_triangle[first_winding ? 3 : 1] == 0u);
    }

    SUBCASE("a slot and generation beyond the old 32-bit bounds survive the GPU target") {
        // 65,536 was v2's first unrepresentable slot and 32 its first generation; v3's maxima
        // exercise the top bit of each word the fragment shader writes.
        for (const auto& [slot, gen] : {std::pair{65536u, 32u},
                                        std::pair{kVirtualGeometryVisibilityV3MaxCluster,
                                                  kVirtualGeometryVisibilityV3MaxGeneration}}) {
            draw.cluster_slot = slot;
            draw.generation = gen;
            const Readback r = render_cluster(*device, pass, request);
            REQUIRE(r.drew);
            const auto id = unpack_virtual_geometry_visibility_id64(at(r.ids, 32, 32));
            REQUIRE(id.has_value());
            CHECK(id->cluster == slot);
            CHECK(id->generation == gen);
            CHECK(id->triangle < 4u);
        }
    }

    SUBCASE("an invalid cluster index draws nothing and is counted") {
        draw.cluster = 99;
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_invalid_request == 1);
        CHECK(pass.stats().drawn == 0);
    }

    SUBCASE("a cluster outside the selected cut draws nothing and is counted") {
        draw.cluster = 0; // the coarse group was replaced by its leaf
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_not_selected == 1);
    }

    SUBCASE("a selected non-leaf group draws nothing and is counted") {
        const VirtualGeometrySelection coarse{{0}, 0};
        request.selection = &coarse;
        draw.cluster = 0;
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
        draw.cluster_slot = kVirtualGeometryVisibilityV3MaxCluster + 1;
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_bad_id == 1);
    }

    SUBCASE("a drawn frame followed by a rejected one is cleared, not stale") {
        (void)render_cluster(*device, pass, request);
        draw.cluster = 99;
        const Readback r = render_cluster(*device, pass, request);
        check_all_clear(r);
        CHECK(pass.stats().drawn == 1);
        CHECK(pass.stats().skipped_invalid_request == 1);
    }
}

namespace {

// Two leaf clusters sharing ONE page, the layout a real cooker emits: both clusters' vertices
// first, then both clusters' cluster-local indices. Cluster 2 therefore lives at a nonzero
// vertex_offset (128 bytes) and first_index (12), which is what exercises the pass's vertex slice
// and index decode. Left quad x ∈ [-0.75, -0.25] → pixels [8, 24); right quad x ∈ [0.25, 0.75] →
// pixels [40, 56); both y ∈ [-0.5, 0.5] → pixels [16, 48). Winding is doubled as above.
assets::VirtualGeometryAsset shared_page_fixture(std::uint32_t bad_index = 0) {
    constexpr std::uint32_t stride = 32;
    const auto quad = [](float x0, float x1) {
        return std::array<std::array<float, 3>, 4>{
            {{x0, -0.5f, 0.5f}, {x1, -0.5f, 0.5f}, {x1, 0.5f, 0.5f}, {x0, 0.5f, 0.5f}}};
    };
    const std::array<std::array<std::array<float, 3>, 4>, 3> quads = {
        quad(-0.5f, 0.5f), quad(-0.75f, -0.25f), quad(0.25f, 0.75f)};
    const std::uint32_t local_indices[6] = {0, 1, 2, 0, 2, 3};

    assets::VirtualGeometryAsset asset{};
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = assets::expected_vertex_stride(asset.attribs);
    const auto append_vertices = [&](const std::array<std::array<float, 3>, 4>& q) {
        for (const auto& p : q) {
            const std::size_t at_byte = asset.page_bytes.size();
            asset.page_bytes.resize(at_byte + stride);
            std::memcpy(asset.page_bytes.data() + at_byte, p.data(), sizeof(float) * 3);
        }
    };
    const auto append_indices = [&](bool reversed, std::uint32_t poison) {
        for (std::uint32_t i = 0; i < 6; ++i) {
            std::uint32_t index = reversed ? local_indices[5 - i] : local_indices[i];
            if (poison != 0 && i == 0) {
                index = poison;
            }
            const std::size_t at_byte = asset.page_bytes.size();
            asset.page_bytes.resize(at_byte + sizeof(index));
            std::memcpy(asset.page_bytes.data() + at_byte, &index, sizeof(index));
        }
    };

    // Page 0: the permanent coarse quad (cluster 0), 12 indices from index 0.
    append_vertices(quads[0]);
    append_indices(false, 0);
    append_indices(true, 0);
    const auto page0_size = static_cast<std::uint32_t>(asset.page_bytes.size());
    // Page 1: clusters 1 and 2. Vertices [1][2], then indices: cluster 1 = [0, 12),
    // cluster 2 = [12, 24) — so cluster 2's first_index is 12 and vertex_offset is 128.
    append_vertices(quads[1]);
    append_vertices(quads[2]);
    append_indices(false, 0);
    append_indices(true, 0);
    append_indices(false, bad_index);
    append_indices(true, 0);
    const auto page1_size = static_cast<std::uint32_t>(asset.page_bytes.size()) - page0_size;

    asset.pages = {{0, page0_size, 0, 1, 0, 0, true}, {page0_size, page1_size, 1, 2, 0, 0, false}};
    const std::array<std::array<std::uint32_t, 4>, 3> layout = {{
        // page, vertex_offset (bytes), first_index, group
        {0, 0, 0, 0},
        {1, 0, 0, 1},
        {1, 4 * stride, 12, 1},
    }};
    for (std::uint32_t i = 0; i < 3; ++i) {
        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {-1.0f, -1.0f, 0.5f};
        cluster.bounds.max = {1.0f, 1.0f, 0.5f};
        cluster.page = layout[i][0];
        cluster.vertex_offset = layout[i][1];
        cluster.first_index = layout[i][2];
        cluster.vertex_count = 4;
        cluster.index_count = 12;
        cluster.replacement_group = layout[i][3];
        asset.clusters.push_back(cluster);
    }
    asset.groups = {{0, 1, 0, 1, 2.0f, true}, {1, 2, 0, 0, 0.1f, false}};
    asset.child_groups = {1};
    asset.coarse_group = 0;
    return asset;
}

std::size_t count_in_rect(const std::vector<VirtualGeometryVisibilityWords>& ids,
                          VirtualGeometryVisibilityWords id,
                          std::uint32_t x0,
                          std::uint32_t x1,
                          std::uint32_t y0,
                          std::uint32_t y1) {
    std::size_t n = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            n += cluster_bits(at(ids, x, y)) == id ? 1 : 0;
        }
    }
    return n;
}

} // namespace

TEST_CASE("vg visibility: a cluster at nonzero page offsets draws its own pixels (M18.1)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping visibility pass proofs");
        return;
    }

    SUBCASE("the second cluster of a shared page covers exactly its own rectangle") {
        const assets::VirtualGeometryAsset asset = shared_page_fixture();
        REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);
        VirtualGeometryResidency residency;
        REQUIRE(residency.register_asset(kAssetId, asset));
        REQUIRE(residency.request_page(kAssetId, 1));
        REQUIRE(residency.complete_page(kAssetId, 1));
        const VirtualGeometrySelection selection{{1}, 0};

        VirtualGeometryVisibilityRequest request{};
        request.asset = &asset;
        request.asset_id = kAssetId;
        request.residency = &residency;
        request.selection = &selection;
        VirtualGeometryClusterDraw draw{2, 11, 2};
        request.clusters = {&draw, 1};
        request.clip_from_object = core::identity();
        const VirtualGeometryVisibilityWords id = *pack_virtual_geometry_visibility_id64({11, 2});

        VirtualGeometryVisibilityPass pass(*device);
        const Readback right = render_cluster(*device, pass, request);
        CHECK(right.drew);
        CHECK(count_in_rect(right.ids, id, 40, 56, 16, 48) == 16u * 32u);
        std::size_t total = 0;
        for (const VirtualGeometryVisibilityWords v : right.ids) {
            total += v == kInvalidVirtualGeometryVisibilityWords ? 0 : 1;
        }
        CHECK(total == 16u * 32u); // nothing from cluster 1's vertices or the coarse page
        CHECK(at(right.depth_bits, 48, 32) == std::bit_cast<std::uint32_t>(0.5f));
        CHECK(at(right.ids, 16, 32) ==
              kInvalidVirtualGeometryVisibilityWords); // cluster 1's rectangle stays clear

        // The sibling at offset zero, drawn from the same page, lands on the other rectangle —
        // the pair proves the offset, not just "something was drawn".
        draw.cluster = 1;
        const Readback left = render_cluster(*device, pass, request);
        CHECK(count_in_rect(left.ids, id, 8, 24, 16, 48) == 16u * 32u);
        CHECK(at(left.ids, 48, 32) == kInvalidVirtualGeometryVisibilityWords);
        CHECK(pass.stats().drawn == 2);
    }

    SUBCASE("an index past the cluster's vertices is rejected, counted, and draws nothing") {
        // validate_virtual_geometry checks cluster/page ranges but never decodes index values,
        // so a cooked index of 4 in a 4-vertex cluster reaches the pass; only its gate stops it.
        const assets::VirtualGeometryAsset asset = shared_page_fixture(4);
        REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);
        VirtualGeometryResidency residency;
        REQUIRE(residency.register_asset(kAssetId, asset));
        REQUIRE(residency.request_page(kAssetId, 1));
        REQUIRE(residency.complete_page(kAssetId, 1));
        const VirtualGeometrySelection selection{{1}, 0};

        VirtualGeometryVisibilityRequest request{};
        request.asset = &asset;
        request.asset_id = kAssetId;
        request.residency = &residency;
        request.selection = &selection;
        const VirtualGeometryClusterDraw draw{2, 11, 0};
        request.clusters = {&draw, 1};
        request.clip_from_object = core::identity();

        VirtualGeometryVisibilityPass pass(*device);
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_bad_index == 1);
        CHECK(pass.stats().drawn == 0);
    }
}

TEST_CASE("vg visibility: indexed-indirect submission with multiple clusters (M18.3b)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping indexed-indirect visibility proofs");
        return;
    }

    const assets::VirtualGeometryAsset asset = shared_page_fixture();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);
    VirtualGeometryResidency residency;
    REQUIRE(residency.register_asset(kAssetId, asset));
    REQUIRE(residency.request_page(kAssetId, 1));
    REQUIRE(residency.complete_page(kAssetId, 1));
    const VirtualGeometrySelection selection{{1}, 0};

    VirtualGeometryVisibilityRequest request{};
    request.asset = &asset;
    request.asset_id = kAssetId;
    request.residency = &residency;
    request.selection = &selection;
    request.clip_from_object = core::identity();

    SUBCASE("two clusters in one request each land their own slot/generation/triangle IDs") {
        std::array<VirtualGeometryClusterDraw, 2> draws = {
            VirtualGeometryClusterDraw{1, 11, 2},
            VirtualGeometryClusterDraw{2, 12, 3},
        };
        request.clusters = {draws.data(), draws.size()};
        const VirtualGeometryVisibilityWords left_id =
            *pack_virtual_geometry_visibility_id64({11, 2});
        const VirtualGeometryVisibilityWords right_id =
            *pack_virtual_geometry_visibility_id64({12, 3});

        VirtualGeometryVisibilityPass pass(*device);
        const Readback r = render_cluster(*device, pass, request);
        CHECK(r.drew);
        CHECK(pass.stats().drawn == 2);
        CHECK(count_in_rect(r.ids, left_id, 8, 24, 16, 48) == 16u * 32u);
        CHECK(count_in_rect(r.ids, right_id, 40, 56, 16, 48) == 16u * 32u);

        // Triangle bits are real and per-cluster: each quad emits four triangles; exactly one
        // winding (two triangles) survives the back-face cull for each cluster.
        for (const auto& id : {left_id, right_id}) {
            std::array<std::size_t, 4> per_triangle{};
            for (const VirtualGeometryVisibilityWords w : r.ids) {
                if (cluster_bits(w) == id) {
                    ++per_triangle[unpack_virtual_geometry_visibility_id64(w)->triangle & 3u];
                }
            }
            CHECK(per_triangle[0] + per_triangle[1] + per_triangle[2] + per_triangle[3] > 0u);
            const bool first_winding = per_triangle[0] != 0;
            CHECK(per_triangle[first_winding ? 0 : 2] > 0u);
            CHECK(per_triangle[first_winding ? 1 : 3] > 0u);
            CHECK(per_triangle[first_winding ? 2 : 0] + per_triangle[first_winding ? 3 : 1] == 0u);
        }
    }

    SUBCASE(
        "a request larger than the fixed capacity counts the overflow and still draws what fits") {
        std::vector<VirtualGeometryClusterDraw> many;
        many.reserve(kVirtualGeometryMaxIndirectDraws + 1);
        for (std::uint32_t slot = 0; slot < kVirtualGeometryMaxIndirectDraws + 1; ++slot) {
            many.push_back({1, slot, 0});
        }
        request.clusters = {many.data(), many.size()};
        const VirtualGeometryVisibilityWords first_id =
            *pack_virtual_geometry_visibility_id64({0, 0});

        VirtualGeometryVisibilityPass pass(*device);
        const Readback r = render_cluster(*device, pass, request);
        CHECK(r.drew);
        CHECK(pass.stats().drawn == kVirtualGeometryMaxIndirectDraws);
        CHECK(pass.stats().skipped_over_capacity == 1);
        // The first accepted cluster is the one that survives the depth test (same depth,
        // earlier draw wins the Less test), so its slot is visible in the left rectangle.
        CHECK(count_in_rect(r.ids, first_id, 8, 24, 16, 48) == 16u * 32u);
    }
}

namespace {

// Read what the builder decided. This is a TEST-ONLY readback: the frame path never waits on these
// counters, which is the property M18.3c exists to deliver (ADR-0043 gate 4).
VirtualGeometryGpuBuildCounters build_counters(rhi::Device& device,
                                               const VirtualGeometryVisibilityPass& pass) {
    VirtualGeometryGpuBuildCounters c{};
    const rhi::BufferHandle handle = pass.cluster_buffers().build_counters;
    if (handle.is_valid()) {
        device.read_buffer(handle, &c, sizeof(c), 0);
    }
    return c;
}

} // namespace

TEST_CASE("vg visibility: the indirect commands are built on the GPU from the cut (M18.3c)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping GPU-built submission proofs");
        return;
    }

    const assets::VirtualGeometryAsset asset = shared_page_fixture();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);
    VirtualGeometryResidency residency;
    REQUIRE(residency.register_asset(kAssetId, asset));
    REQUIRE(residency.request_page(kAssetId, 1));
    REQUIRE(residency.complete_page(kAssetId, 1));

    // One selection, computed on the GPU, whose per-group verdict STAYS on the GPU. The CPU copy
    // returned here is only what the assertions and the CPU-path comparison need.
    const VirtualGeometrySelectionInput input{.pixels_per_metre = 2.0f,
                                              .max_projected_error_px = 1.0f,
                                              .page_resident =
                                                  residency.page_residency_bytes(kAssetId)};
    VirtualGeometryGpuSelectionBuffers flags;
    const VirtualGeometrySelection selection =
        select_virtual_geometry_on_gpu(*device, asset, input, &flags);
    REQUIRE(selection.groups == std::vector<std::uint32_t>{1});
    REQUIRE(flags.selected_flags.is_valid());
    REQUIRE(flags.group_count == 2);

    // Cluster 0 is the coarse quad (pixels [16,48)²) and belongs to group 0, which the cut did NOT
    // select; clusters 1 and 2 are the selected leaves (left [8,24), right [40,56)). Offering all
    // three is the point: only the GPU's verdict keeps the coarse quad out of the picture.
    std::array<VirtualGeometryClusterDraw, 3> offered = {
        VirtualGeometryClusterDraw{0, 10, 1},
        VirtualGeometryClusterDraw{1, 11, 2},
        VirtualGeometryClusterDraw{2, 12, 3},
    };
    const VirtualGeometryVisibilityWords coarse_id =
        *pack_virtual_geometry_visibility_id64({10, 1});
    const VirtualGeometryVisibilityWords left_id = *pack_virtual_geometry_visibility_id64({11, 2});
    const VirtualGeometryVisibilityWords right_id = *pack_virtual_geometry_visibility_id64({12, 3});

    VirtualGeometryVisibilityRequest request{};
    request.asset = &asset;
    request.asset_id = kAssetId;
    request.residency = &residency;
    request.selection = &selection;
    request.clusters = {offered.data(), offered.size()};
    request.clip_from_object = core::identity();

    SUBCASE("the GPU-built draw list renders the same pixels as the CPU-built one") {
        // The strongest available proof that the builder applied the cut correctly: the same
        // request through both paths must produce IDENTICAL images. A builder that ignored the
        // verdict would paint the coarse quad over the middle; one that mis-ranked its slots would
        // swap the two leaves' IDs. (It does NOT cover a stale tail: the buffers are still
        // re-created zero-filled per declare(), so nothing stale exists to replay yet — see the
        // shader's note.)
        VirtualGeometryVisibilityPass cpu_pass(*device);
        const Readback cpu = render_cluster(*device, cpu_pass, request);
        CHECK(cpu.drew);
        CHECK(cpu_pass.stats().drawn == 2);
        CHECK(cpu_pass.stats().skipped_not_selected == 1); // the coarse cluster, gated on the CPU
        CHECK(cpu_pass.stats().gpu_built == 0);

        request.gpu_selection = &flags;
        VirtualGeometryVisibilityPass gpu_pass(*device);
        const Readback gpu = render_cluster(*device, gpu_pass, request);
        CHECK(gpu.drew);
        CHECK(gpu_pass.stats().gpu_built == 1);
        // `drawn` is deliberately 0 here: the CPU does not know what was drawn on this path.
        CHECK(gpu_pass.stats().drawn == 0);
        CHECK(gpu_pass.stats().candidates_offered == 3);   // the coarse cluster IS offered…
        CHECK(gpu_pass.stats().skipped_not_selected == 0); // …and the CPU no longer judges it
        CHECK(gpu_pass.stats().skipped_not_leaf == 0);     // the coarse exception applied

        const VirtualGeometryGpuBuildCounters counters = build_counters(*device, gpu_pass);
        CHECK(counters.selected_total == 2); // the GPU found exactly the two leaves
        CHECK(counters.emitted == 2);
        CHECK(counters.fell_back_to_coarse == 0);
        CHECK(counters.coarse_over_capacity == 0);

        CHECK(gpu.ids == cpu.ids);
        CHECK(gpu.depth_bits == cpu.depth_bits);
        // Stated absolutely as well as relatively, so the pair cannot both be empty and "agree".
        CHECK(count_in_rect(gpu.ids, left_id, 8, 24, 16, 48) == 16u * 32u);
        CHECK(count_in_rect(gpu.ids, right_id, 40, 56, 16, 48) == 16u * 32u);
        std::size_t coarse_pixels = 0;
        for (const VirtualGeometryVisibilityWords w : gpu.ids) {
            coarse_pixels += cluster_bits(w) == coarse_id ? 1 : 0;
        }
        CHECK(coarse_pixels == 0); // the unselected group never reached the raster stage
        CHECK(at(gpu.ids, 32, 32) == kInvalidVirtualGeometryVisibilityWords); // between the leaves
    }

    SUBCASE("a cut that overflows the capacity degrades to the coarse cut, not to a hole") {
        // max_draws = 1 makes the two-leaf cut overflow with three clusters instead of 1025. The
        // submitted draw count is unchanged; only the builder's capacity moved.
        request.gpu_selection = &flags;
        request.max_draws = 1;

        VirtualGeometryVisibilityPass pass(*device);
        const Readback r = render_cluster(*device, pass, request);
        CHECK(r.drew);
        CHECK(pass.stats().gpu_built == 1);
        CHECK(pass.stats().candidates_offered == 3);
        CHECK(pass.stats().skipped_over_candidate_cap ==
              0); // the cap is the builder's, not the CPU's

        const VirtualGeometryGpuBuildCounters counters = build_counters(*device, pass);
        CHECK(counters.selected_total == 2);
        CHECK(counters.fell_back_to_coarse == 1);
        CHECK(counters.emitted == 1);
        CHECK(counters.coarse_over_capacity == 0);
        CHECK(counters.overflow_without_coarse == 0);

        // What landed is the COARSE quad covering [16,48)², whole — not one leaf and a hole where
        // the other should be. The leaves' exclusive columns are the witness: pixels in [8,16) and
        // [48,56) belong to a leaf only, so they must be clear.
        CHECK(count_in_rect(r.ids, coarse_id, 16, 48, 16, 48) == 32u * 32u);
        CHECK(count_in_rect(r.ids, left_id, 8, 16, 16, 48) == 0u);
        CHECK(count_in_rect(r.ids, right_id, 48, 56, 16, 48) == 0u);
        std::size_t drawn_pixels = 0;
        for (const VirtualGeometryVisibilityWords w : r.ids) {
            drawn_pixels += w == kInvalidVirtualGeometryVisibilityWords ? 0 : 1;
        }
        CHECK(drawn_pixels == 32u * 32u); // exactly the coarse quad, nothing else
    }

    SUBCASE("an overflow with no coarse candidate offered draws nothing and says so") {
        // The fallback can only fire if the coarse clusters were offered. A caller that offers only
        // leaves and then overflows gets nothing drawn — which is defensible, but must be VISIBLE,
        // or an object vanishes with every counter reading zero.
        request.gpu_selection = &flags;
        request.max_draws = 1;
        request.clusters = {offered.data() + 1, 2}; // the two leaves, no coarse cluster

        VirtualGeometryVisibilityPass pass(*device);
        const Readback r = render_cluster(*device, pass, request);
        CHECK(pass.stats().candidates_offered == 2);
        const VirtualGeometryGpuBuildCounters counters = build_counters(*device, pass);
        CHECK(counters.selected_total == 2);
        CHECK(counters.fell_back_to_coarse == 1);
        CHECK(counters.overflow_without_coarse == 1); // the witness
        CHECK(counters.emitted == 0);
        check_all_clear(r); // nothing was drawn, and the targets are "nothing" rather than stale
    }

    SUBCASE("a verdict buffer that carries nothing refuses the request instead of guessing") {
        const VirtualGeometryGpuSelectionBuffers empty{};
        request.gpu_selection = &empty;

        VirtualGeometryVisibilityPass pass(*device);
        const Readback r = render_cluster(*device, pass, request);
        CHECK_FALSE(r.drew);
        check_all_clear(r);
        CHECK(pass.stats().skipped_no_gpu_verdict == 3);
        CHECK(pass.stats().drawn == 0);
        CHECK(pass.stats().candidates_offered == 0);
        CHECK(pass.stats().gpu_built == 0);
    }

    SUBCASE("the build is a compute pass in the same frame as the draw it feeds") {
        // The structural half of "no readback decides this frame's draw list": the command build
        // and the raster pass are two passes of ONE graph, ordered by the graph's own dependency
        // derivation rather than by a CPU wait in between.
        request.gpu_selection = &flags;
        RenderGraph graph(*device);
        const RGTexture ids =
            graph.create_texture({{kSize, kSize}, rhi::Format::RG32Uint, "vg-ids"});
        const RGTexture depth_bits =
            graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-depth-bits"});
        const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "vg-depth"});
        graph.export_texture(ids);

        VirtualGeometryVisibilityPass pass(*device);
        REQUIRE(pass.declare(graph, ids, depth_bits, depth, request));
        REQUIRE(graph.pass_count() == 2);
        CHECK(graph.pass_name(0) == "vg-build-draws");
        CHECK(graph.pass_name(1) == "vg-visibility");

        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        CHECK(build_counters(*device, pass).emitted == 2);
    }

    device->destroy(flags.selected_flags);
}
