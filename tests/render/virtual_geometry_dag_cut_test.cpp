// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M18.6 cross-language proof: the Rust DAG cook's committed output (tests/assets/fixtures/
// vg_dag_sphere.rvg — a closed two-material sphere, regenerated only deliberately, see
// tools/asset-pipeline/src/virtual_geometry_dag/tests.rs) is read by the real C++ reader and cut by
// the real select_virtual_geometry(). At every view scale and under partial page residency the cut
// must be a valid replacement cut — no group twice, every leaf covered by exactly one selected
// ancestor-or-self — and WATERTIGHT: decoded from the page bytes, every edge of the selected
// triangles (compared by exact position bits) is shared by exactly two triangles. The GPU twin
// must select the same set.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/render/virtual_geometry_gpu_selection.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/rhi/rhi.hpp"

using namespace rime;

namespace {

using Position = std::array<std::uint32_t, 3>; // exact f32 bit patterns
using Edge = std::pair<Position, Position>;

[[nodiscard]] assets::VirtualGeometryAsset load_fixture() {
    const std::filesystem::path path =
        std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / "vg_dag_sphere.rvg";
    const auto bytes = platform::read_file(path);
    REQUIRE_MESSAGE(bytes.has_value(), "missing committed fixture ", path.string());
    assets::AssetError error{};
    auto asset = assets::read_virtual_geometry(*bytes, error);
    REQUIRE_MESSAGE(asset.has_value(), assets::to_string(error));
    return *asset;
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t at) {
    return std::to_integer<std::uint32_t>(bytes[at]) |
           (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8) |
           (std::to_integer<std::uint32_t>(bytes[at + 2]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[at + 3]) << 24);
}

// Decode one cluster's triangles through the reader's own checked page view.
void append_triangles(const assets::VirtualGeometryAsset& asset,
                      std::uint32_t cluster,
                      std::vector<std::array<Position, 3>>& out) {
    assets::VirtualGeometryPageView view{};
    REQUIRE(assets::view_virtual_geometry_page(asset, cluster, view) ==
            assets::VirtualGeometryPageViewError::None);
    for (std::uint32_t t = 0; t < view.index_count / 3; ++t) {
        std::array<Position, 3> triangle{};
        for (std::uint32_t k = 0; k < 3; ++k) {
            std::uint32_t local = 0;
            REQUIRE(assets::read_virtual_geometry_index(view, 3 * t + k, local));
            REQUIRE(local < view.vertex_count);
            const std::size_t at = view.vertex_offset + std::size_t{local} * view.vertex_stride;
            triangle[k] = {read_u32(view.vertices, at),
                           read_u32(view.vertices, at + 4),
                           read_u32(view.vertices, at + 8)};
        }
        out.push_back(triangle);
    }
}

struct Topology {
    std::vector<std::uint32_t> parent; // kInvalidVirtualGeometryIndex for the root
    std::vector<bool> leaf;
};

[[nodiscard]] Topology topology(const assets::VirtualGeometryAsset& asset) {
    Topology topo{
        std::vector<std::uint32_t>(asset.groups.size(), assets::kInvalidVirtualGeometryIndex),
        std::vector<bool>(asset.groups.size(), false)};
    for (std::uint32_t g = 0; g < asset.groups.size(); ++g) {
        const assets::VirtualGeometryGroup& group = asset.groups[g];
        topo.leaf[g] = group.child_count == 0;
        for (std::uint32_t i = 0; i < group.child_count; ++i) {
            const std::uint32_t child = asset.child_groups[group.first_child + i];
            // One parent per group: the shape the tree-walking selectors need to never select a
            // group twice. (The cook still swaps whole simplification groups atomically.)
            REQUIRE(topo.parent[child] == assets::kInvalidVirtualGeometryIndex);
            topo.parent[child] = g;
        }
    }
    return topo;
}

// Returns the number of selected triangles.
std::size_t check_cut(const assets::VirtualGeometryAsset& asset,
                      const Topology& topo,
                      const render::VirtualGeometrySelection& selection) {
    REQUIRE(selection.rejected_invalid_input == 0);
    std::vector<bool> chosen(asset.groups.size(), false);
    for (const std::uint32_t g : selection.groups) {
        REQUIRE(g < asset.groups.size());
        CHECK_MESSAGE(!chosen[g], "group ", g, " selected twice");
        chosen[g] = true;
    }
    // Coverage: every leaf has exactly one selected ancestor-or-self, so no parent is drawn with
    // its child and no region is left uncovered.
    for (std::uint32_t g = 0; g < asset.groups.size(); ++g) {
        if (!topo.leaf[g])
            continue;
        int hits = 0;
        for (std::uint32_t at = g; at != assets::kInvalidVirtualGeometryIndex; at = topo.parent[at])
            hits += chosen[at] ? 1 : 0;
        CHECK_MESSAGE(hits == 1, "leaf group ", g, " covered ", hits, " times");
    }
    // Watertightness on the decoded triangles.
    std::vector<std::array<Position, 3>> triangles;
    for (const std::uint32_t g : selection.groups) {
        const assets::VirtualGeometryGroup& group = asset.groups[g];
        for (std::uint32_t c = 0; c < group.cluster_count; ++c)
            append_triangles(asset, group.first_cluster + c, triangles);
    }
    std::map<Edge, int> uses;
    for (const auto& t : triangles) {
        for (int k = 0; k < 3; ++k) {
            const Position a = t[k];
            const Position b = t[(k + 1) % 3];
            ++uses[a < b ? Edge{a, b} : Edge{b, a}];
        }
    }
    int open_or_overlapping = 0;
    for (const auto& [edge, count] : uses)
        open_or_overlapping += count != 2 ? 1 : 0;
    CHECK_MESSAGE(open_or_overlapping == 0,
                  open_or_overlapping,
                  " edges are not shared by exactly two triangles");
    return triangles.size();
}

// Several camera scales from "the whole mesh is a few pixels" to "every error is visible".
[[nodiscard]] std::vector<float> pixels_per_metre_sweep(const assets::VirtualGeometryAsset& asset) {
    const float root_error = asset.groups[asset.coarse_group].lod_error_m;
    REQUIRE(root_error > 0.0f);
    std::vector<float> sweep{0.0f};
    for (float scale = 0.25f; scale <= 4096.0f; scale *= 2.0f)
        sweep.push_back(scale / root_error);
    return sweep;
}

[[nodiscard]] std::vector<std::uint8_t> residency(std::size_t pages, std::uint64_t seed) {
    std::vector<std::uint8_t> out(pages);
    std::uint64_t state = seed * 6364136223846793005ull + 1ull;
    for (std::uint8_t& resident : out) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        resident = ((state >> 33) % 100) < 70 ? 1 : 0;
    }
    return out;
}

} // namespace

TEST_CASE("virtual geometry DAG: the cooked fixture reads, validates, and is multi-level (M18.6)") {
    const assets::VirtualGeometryAsset asset = load_fixture();
    CHECK(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);
    CHECK(asset.groups.size() > 16);
    CHECK(asset.pages.size() > 4);
    const Topology topo = topology(asset);
    int depth = 0;
    for (std::uint32_t g = 0; g < asset.groups.size(); ++g) {
        int d = 0;
        for (std::uint32_t at = g; topo.parent[at] != assets::kInvalidVirtualGeometryIndex;
             at = topo.parent[at])
            ++d;
        depth = std::max(depth, d);
    }
    CHECK(depth >= 3);
    MESSAGE("DAG fixture: ",
            asset.groups.size(),
            " groups, ",
            asset.pages.size(),
            " pages, depth ",
            depth,
            ", root error ",
            asset.groups[asset.coarse_group].lod_error_m,
            " m");
}

TEST_CASE("virtual geometry DAG: select_virtual_geometry() cuts are valid and watertight (M18.6)") {
    const assets::VirtualGeometryAsset asset = load_fixture();
    const Topology topo = topology(asset);

    std::set<std::vector<std::uint32_t>> distinct_cuts;
    std::size_t coarsest = 0;
    std::size_t finest = 0;
    int partial_residency_cuts = 0;
    for (const float ppm : pixels_per_metre_sweep(asset)) {
        const render::VirtualGeometrySelection full = render::select_virtual_geometry(
            asset, {.pixels_per_metre = ppm, .max_projected_error_px = 1.0f, .page_resident = {}});
        // With no residency span only the permanent root page is resident: the coarse fallback.
        CHECK(full.groups == std::vector<std::uint32_t>{asset.coarse_group});

        const std::vector<std::uint8_t> all(asset.pages.size(), 1);
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            asset, {.pixels_per_metre = ppm, .max_projected_error_px = 1.0f, .page_resident = all});
        CHECK(selection.refinement_blocked_by_residency == 0);
        const std::size_t triangles = check_cut(asset, topo, selection);
        if (ppm == 0.0f)
            coarsest = triangles;
        finest = triangles;
        std::vector<std::uint32_t> sorted = selection.groups;
        std::sort(sorted.begin(), sorted.end());
        distinct_cuts.insert(sorted);

        for (std::uint64_t seed = 0; seed < 4; ++seed) {
            const std::vector<std::uint8_t> resident = residency(asset.pages.size(), seed);
            const render::VirtualGeometrySelection partial =
                render::select_virtual_geometry(asset,
                                                {.pixels_per_metre = ppm,
                                                 .max_projected_error_px = 1.0f,
                                                 .page_resident = resident});
            check_cut(asset, topo, partial);
            partial_residency_cuts += partial.refinement_blocked_by_residency != 0 ? 1 : 0;
        }
    }
    // The sweep really moved through the hierarchy: from the root to (at least nearly) the leaves.
    CHECK(distinct_cuts.size() >= 4);
    CHECK(finest > 4 * coarsest);
    CHECK(partial_residency_cuts > 0);
    MESSAGE(distinct_cuts.size(),
            " distinct cuts, ",
            coarsest,
            " -> ",
            finest,
            " triangles; residency blocked refinement in ",
            partial_residency_cuts,
            " cuts");
}

TEST_CASE("virtual geometry DAG: the GPU twin selects the same cut as the oracle (M18.6)") {
    if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
        auto probe = rhi::create_device({});
        if (!probe)
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
    }
    auto device = rhi::create_device({});
    if (!device) {
        MESSAGE("no Vulkan device available — skipping the DAG GPU-twin comparison");
        return;
    }
    const assets::VirtualGeometryAsset asset = load_fixture();
    int compared = 0;
    for (const float ppm : pixels_per_metre_sweep(asset)) {
        for (std::uint64_t seed = 0; seed < 3; ++seed) {
            const std::vector<std::uint8_t> resident =
                seed == 0 ? std::vector<std::uint8_t>(asset.pages.size(), 1)
                          : residency(asset.pages.size(), seed);
            const render::VirtualGeometrySelectionInput input = {
                .pixels_per_metre = ppm, .max_projected_error_px = 1.0f, .page_resident = resident};
            render::VirtualGeometrySelection cpu = render::select_virtual_geometry(asset, input);
            render::VirtualGeometrySelection gpu =
                render::select_virtual_geometry_on_gpu(*device, asset, input);
            std::sort(cpu.groups.begin(), cpu.groups.end());
            std::sort(gpu.groups.begin(), gpu.groups.end());
            CHECK(gpu.groups == cpu.groups);
            CHECK(gpu.refinement_blocked_by_residency == cpu.refinement_blocked_by_residency);
            CHECK(gpu.gpu_depth_overflow == 0);
            CHECK(gpu.gpu_depth_fallback == 0);
            ++compared;
        }
    }
    MESSAGE("GPU twin matched the oracle on ", compared, " DAG cuts");
}
