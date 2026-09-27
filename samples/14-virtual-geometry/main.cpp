// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// 14-virtual-geometry — the COMPLEXITY SWEEP behind ADR-0043 gate 4.
//
// Gate 4 asks for two things. M18.3b/c built the first: GPU selection plus indirect submission
// agreeing with the CPU oracle, with no readback deciding the frame's draw list. This sample is the
// second: "complexity sweeps report candidates, selected triangles, CPU submission and GPU time at
// a fixed projected size". Those four numbers are the columns below, and this is the only place in
// the tree that produces them — `scripts/perf.sh` had no virtual-geometry scenario at all.
//
// Why a synthetic asset rather than a cooked mesh. The sweep's independent variable is the SIZE OF
// THE CUT, and it must move by itself while everything a number could otherwise be blamed on holds
// still. A cooked mesh couples cut size to silhouette, material count, page layout and residency
// all at once; a generated quadtree moves one thing. The cost is that the absolute milliseconds are
// not a statement about any real asset — they are a statement about how the draw-list machinery
// scales, which is what the gate asks. `99-the-block` remains the sample that speaks for real
// content (gate 7).
//
// THE FIXED PROJECTED SIZE. `pixels_per_metre` and `max_projected_error_px` are constants here
// (kPixelsPerMetre / kMaxErrorPx), and every interior group's LOD error is above the threshold
// while every leaf's is below it. So the cut is always "all the leaves", chosen by the same policy
// a camera would use, and depth alone decides how many there are. If the sweep instead moved the
// camera, a row's candidate count and its projected error would change together and neither column
// would mean anything on its own.
//
// Usage:
//   virtual_geometry --sweep [--out sweep.json] [--frames N]
//   virtual_geometry --perf  [--out r.json] [--baseline b.json] [--frames N] [--depth D]
//
// Like every other `--perf` sample this is NOT a CTest: it reports wall-clock time on one named
// GPU, so it belongs in a fingerprinted `docs/perf/` report, not in a CI gate that runs on
// lavapipe. The machine-independent half — candidates, triangles, emitted draws, every skip
// counter — travels in the report's work ledger, which is what stops a fast run that drew nothing
// from reading as a pass.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rime/assets/virtual_geometry.hpp"
#include "rime/core/diagnostics/perf_report.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/core/diagnostics/work_ledger.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_gpu_selection.hpp"
#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "rime/render/virtual_geometry_visibility_pass.hpp"
#include "rime/rhi/device.hpp"

using namespace rime;

namespace {

// The work ledger borrows its key strings (WorkCounter::name is a string_view, documented as
// expecting literals), so a generated key needs storage that outlives the ledger AND never moves.
// A reused stack buffer silently aliases every entry written through it: the first version of this
// sample printed ten counters that all pointed at the same 64 bytes, so the table was right and the
// ledger beside it was fiction. std::deque is what gives stable addresses across growth.
class KeyArena {
public:
    std::string_view format(const char* fmt, ...) {
        char buffer[96];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        keys_.emplace_back(buffer);
        return keys_.back();
    }

private:
    std::deque<std::string> keys_;
};

constexpr float kPixelsPerMetre = 1.0f;
constexpr float kMaxErrorPx = 1.0f;
// Above the threshold at the fixed projected size, so an interior group always refines…
constexpr float kInteriorLodErrorM = 2.0f;
// …and below it, so a leaf is always on the cut.
constexpr float kLeafLodErrorM = 0.1f;

constexpr std::uint32_t kVertexStride = 32u; // cooked v1: position, normal, uv
constexpr assets::AssetId kAssetId{0x14u};

// ── The generated quadtree ──────────────────────────────────────────────────────────────────────
//
// One replacement group per node, one cluster per group, one page per cluster, every page
// permanently resident. Residency is deliberately NOT a variable of this sweep: gate 5 is the
// streaming gate and owns delayed pages, so leaving every page resident here keeps
// `refinement_blocked_by_residency` at zero and makes any nonzero value a bug rather than a knob.
//
// Each node owns a square of clip space and its four children own its quadrants, so the leaves of
// any depth tile the viewport exactly once. That matters for the GPU column: triangle count and
// covered pixels both scale with the cut instead of a thousand clusters piling into one texel.

struct Tile {
    float x0, y0, x1, y1;
};

// `tris` triangles as one strip across the tile: (tris + 2) vertices, alternating bottom/top edge.
// Odd triangles swap their first two indices so every triangle keeps the same winding — a
// back-face-culled half of a strip would silently halve the raster work the GPU column reports.
std::vector<std::byte> cluster_page(const Tile& tile, std::uint32_t tris) {
    const std::uint32_t vertices = tris + 2u;
    const std::uint32_t columns = (vertices + 1u) / 2u;
    std::vector<std::byte> bytes(static_cast<std::size_t>(vertices) * kVertexStride +
                                 static_cast<std::size_t>(tris) * 3u * sizeof(std::uint32_t));

    for (std::uint32_t v = 0; v < vertices; ++v) {
        const std::uint32_t column = v / 2u;
        const float t =
            (columns > 1u) ? static_cast<float>(column) / static_cast<float>(columns - 1u) : 0.0f;
        const float position[3] = {
            tile.x0 + (tile.x1 - tile.x0) * t, (v % 2u == 0u) ? tile.y0 : tile.y1, 0.5f};
        const float normal[3] = {0.0f, 0.0f, 1.0f};
        const float uv[2] = {t, (v % 2u == 0u) ? 0.0f : 1.0f};
        std::byte* out = bytes.data() + static_cast<std::size_t>(v) * kVertexStride;
        std::memcpy(out, position, sizeof(position));
        std::memcpy(out + 12, normal, sizeof(normal));
        std::memcpy(out + 24, uv, sizeof(uv));
    }

    std::byte* indices = bytes.data() + static_cast<std::size_t>(vertices) * kVertexStride;
    for (std::uint32_t t = 0; t < tris; ++t) {
        const std::uint32_t a = t, b = t + 1u, c = t + 2u;
        const std::uint32_t tri[3] = {(t % 2u == 0u) ? a : b, (t % 2u == 0u) ? b : a, c};
        std::memcpy(
            indices + static_cast<std::size_t>(t) * 3u * sizeof(std::uint32_t), tri, sizeof(tri));
    }
    return bytes;
}

struct Quadtree {
    assets::VirtualGeometryAsset asset;
    std::vector<std::uint32_t> leaf_groups;  // the cut, at the fixed projected size
    std::uint32_t triangles_per_cluster = 0; // every cluster carries the same count
};

// Depth 0 is the root alone. Depth d has 4^d leaves and (4^(d+1) - 1) / 3 groups.
Quadtree build_quadtree(std::uint32_t depth, std::uint32_t tris) {
    Quadtree out;
    out.triangles_per_cluster = tris;
    assets::VirtualGeometryAsset& asset = out.asset;
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = assets::expected_vertex_stride(asset.attribs);
    asset.coarse_group = 0;

    struct Node {
        Tile tile;
        std::uint32_t level;
    };

    // Breadth-first, so a parent's `first_child` block is contiguous — which is the layout
    // VirtualGeometryGroup's (first_child, child_count) range assumes.
    std::vector<Node> nodes{Node{{-1.0f, -1.0f, 1.0f, 1.0f}, 0}};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const Node node = nodes[index];
        const bool leaf = node.level == depth;

        const std::vector<std::byte> page = cluster_page(node.tile, tris);
        const auto offset = static_cast<std::uint64_t>(asset.page_bytes.size());
        asset.page_bytes.insert(asset.page_bytes.end(), page.begin(), page.end());

        assets::VirtualGeometryPage page_record{};
        page_record.byte_offset = offset;
        page_record.byte_size = static_cast<std::uint32_t>(page.size());
        page_record.first_cluster = static_cast<std::uint32_t>(index);
        page_record.cluster_count = 1;
        page_record.first_dependency = 0;
        page_record.dependency_count = 0;
        page_record.permanently_resident = true;
        asset.pages.push_back(page_record);

        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {node.tile.x0, node.tile.y0, 0.5f};
        cluster.bounds.max = {node.tile.x1, node.tile.y1, 0.5f};
        cluster.page = static_cast<std::uint32_t>(index);
        cluster.vertex_count = tris + 2u;
        cluster.index_count = tris * 3u;
        cluster.replacement_group = static_cast<std::uint32_t>(index);
        asset.clusters.push_back(cluster);

        assets::VirtualGeometryGroup group{};
        group.first_cluster = static_cast<std::uint32_t>(index);
        group.cluster_count = 1;
        group.lod_error_m = leaf ? kLeafLodErrorM : kInteriorLodErrorM;
        group.permanently_resident = true;
        group.child_count = leaf ? 0u : 4u;
        group.first_child = static_cast<std::uint32_t>(asset.child_groups.size());
        asset.groups.push_back(group);

        if (leaf) {
            out.leaf_groups.push_back(static_cast<std::uint32_t>(index));
            continue;
        }
        // Reserve the child slots now and fill them as the children are appended below, so the
        // block stays contiguous even though the children's indices are not yet known.
        const std::size_t first_child_slot = asset.child_groups.size();
        asset.child_groups.resize(first_child_slot + 4u, 0u);
        const float mid_x = 0.5f * (node.tile.x0 + node.tile.x1);
        const float mid_y = 0.5f * (node.tile.y0 + node.tile.y1);
        const Tile quadrants[4] = {{node.tile.x0, node.tile.y0, mid_x, mid_y},
                                   {mid_x, node.tile.y0, node.tile.x1, mid_y},
                                   {node.tile.x0, mid_y, mid_x, node.tile.y1},
                                   {mid_x, mid_y, node.tile.x1, node.tile.y1}};
        for (std::uint32_t q = 0; q < 4u; ++q) {
            asset.child_groups[first_child_slot + q] = static_cast<std::uint32_t>(nodes.size());
            nodes.push_back(Node{quadrants[q], node.level + 1u});
        }
    }
    return out;
}

// ── One measured frame ──────────────────────────────────────────────────────────────────────────

struct FrameCost {
    double select_ms = 0.0; // GPU selection, including the blocking submit it still does
    double build_ms = 0.0;  // assembling the candidate array the builder consumes
    double submit_ms = 0.0; // graph declare + record + submit + wait
    double gpu_ms = 0.0;    // summed vg-* pass timestamps
    std::vector<core::PassTiming> passes;
};

struct LevelResult {
    std::uint32_t depth = 0;
    std::uint32_t groups = 0;
    std::uint32_t leaves = 0;
    std::uint32_t candidates = 0;     // offered by ONE declare(), not the pass's running total
    std::uint32_t uploaded = 0;       // what the CPU handed over, for the two to be compared
    std::uint64_t covered_pixels = 0; // nonzero visibility IDs in the first measured frame
    std::uint64_t selected_triangles = 0;
    core::Distribution cpu;    // select + build + submit
    core::Distribution select; // the selection dispatch's own blocking submit
    core::Distribution build;  // assembling the candidate array
    core::Distribution submit; // graph declare + record + submit + wait — gate 4's "CPU submission"
    core::Distribution gpu;    // summed pass timestamps
    render::VirtualGeometryGpuBuildCounters counters{};
    render::VirtualGeometryVisibilityStats stats{};
    std::uint32_t refinement_blocked = 0;
    std::uint32_t skipped_over_candidate_cap = 0; // per frame, not the pass's running total
    std::uint64_t frames_without_raster = 0; // must be 0, or the GPU column is measuring a cull
    bool measured = false;
};

// The candidate array: every leaf cluster on the cut, plus the coarse cluster, which the GPU path
// accepts despite the leaf-only rule because the overflow fallback must already be on the device
// when the builder discovers the overflow.
void build_candidates(const Quadtree& tree,
                      const render::VirtualGeometrySelection& cut,
                      std::vector<render::VirtualGeometryClusterDraw>& out) {
    out.clear();
    out.reserve(cut.groups.size() + 1u);
    std::uint32_t slot = 0;
    const auto push = [&](std::uint32_t group) {
        const assets::VirtualGeometryGroup& g = tree.asset.groups[group];
        for (std::uint32_t c = 0; c < g.cluster_count; ++c)
            out.push_back({g.first_cluster + c, slot++, 1u});
    };
    push(tree.asset.coarse_group);
    for (const std::uint32_t group : cut.groups) {
        if (group != tree.asset.coarse_group)
            push(group);
    }
}

std::uint64_t triangles_on_cut(const Quadtree& tree, const render::VirtualGeometrySelection& cut) {
    std::uint64_t total = 0;
    for (const std::uint32_t group : cut.groups) {
        const assets::VirtualGeometryGroup& g = tree.asset.groups[group];
        for (std::uint32_t c = 0; c < g.cluster_count; ++c)
            total += tree.asset.clusters[g.first_cluster + c].index_count / 3u;
    }
    return total;
}

render::VirtualGeometryGpuBuildCounters
read_counters(rhi::Device& device, const render::VirtualGeometryVisibilityPass& pass) {
    render::VirtualGeometryGpuBuildCounters counters{};
    const rhi::BufferHandle handle = pass.cluster_buffers().build_counters;
    if (handle.is_valid())
        device.read_buffer(handle, &counters, sizeof(counters));
    return counters;
}

// Measure `frames` frames of the GPU-built draw path at one complexity level. `width`/`height` fix
// the raster area so two rows differ only in how the same viewport is subdivided.
LevelResult run_level(rhi::Device& device,
                      std::uint32_t depth,
                      std::uint32_t tris,
                      int frames,
                      int warmup,
                      std::uint32_t width,
                      std::uint32_t height,
                      core::PerfReport* report,
                      const char* timeline_prefix,
                      std::uint32_t max_draws = 0) {
    LevelResult result;
    result.depth = depth;

    const Quadtree tree = build_quadtree(depth, tris);
    result.groups = static_cast<std::uint32_t>(tree.asset.groups.size());
    result.leaves = static_cast<std::uint32_t>(tree.leaf_groups.size());
    if (assets::validate_virtual_geometry(tree.asset) != assets::VirtualGeometryError::None) {
        std::fprintf(
            stderr, "14-virtual-geometry: generated asset at depth %u is invalid\n", depth);
        return result;
    }

    // Every page is permanently resident, so registration is all the cache needs: a permanent
    // page is resident by contract, which is why `input.page_resident` stays empty below.
    render::VirtualGeometryResidency residency;
    if (!residency.register_asset(kAssetId, tree.asset)) {
        std::fprintf(stderr, "14-virtual-geometry: residency rejected the generated asset\n");
        return result;
    }
    render::VirtualGeometryVisibilityPass pass(device);

    render::VirtualGeometrySelectionInput input{};
    input.pixels_per_metre = kPixelsPerMetre;
    input.max_projected_error_px = kMaxErrorPx;

    core::DurationSamples cpu, select_s, build_s, submit_s, gpu;
    std::vector<render::VirtualGeometryClusterDraw> candidates;

    for (int frame = 0; frame < warmup + frames; ++frame) {
        const bool measuring = frame >= warmup;
        FrameCost cost;

        const core::Stopwatch select_watch;
        render::VirtualGeometryGpuSelectionBuffers flags{};
        const render::VirtualGeometrySelection cut =
            render::select_virtual_geometry_on_gpu(device, tree.asset, input, &flags);
        cost.select_ms = select_watch.elapsed_ms();

        const core::Stopwatch build_watch;
        build_candidates(tree, cut, candidates);
        cost.build_ms = build_watch.elapsed_ms();

        const core::Stopwatch submit_watch;
        render::RenderGraph graph(device);
        const render::RGTexture ids =
            graph.create_texture({{width, height}, rhi::Format::RG32Uint, "vg-ids"});
        const render::RGTexture depth_bits =
            graph.create_texture({{width, height}, rhi::Format::R32Uint, "vg-depth-bits"});
        const render::RGTexture depth_target =
            graph.create_texture({{width, height}, render::kDepthFormat, "vg-depth"});
        // EXPORT EVERY FRAME, AND NOT ONLY TO READ THE PIXELS BACK. The render graph culls passes
        // whose outputs nothing consumes, so a frame that does not export the visibility target
        // never rasterizes at all — and the compute builder, which writes an imported buffer,
        // survives the cull and keeps reporting a time. The first version of this sample exported
        // only on one frame and so measured 119 frames of a dead raster pass: it reported 0.015 ms
        // at both 720p and 1080p, identical to four significant figures, which is the tell. The
        // numbers were a real measurement of nothing.
        graph.export_texture(ids);
        graph.export_texture(depth_bits);
        // Reading the pixels back is a separate, once-per-level thing: it is the GPU column's
        // vacuity guard, because a raster pass that covers no pixels is the cheapest one there is.
        //
        // It happens on the LAST WARMUP frame, never on a measured one. A full-framebuffer copy is
        // 16 MB at 1080p, and it is recorded inside the same command buffer the submit timer
        // covers: on a measured frame it would land as a single outlier, and nearest-rank p99 of
        // 120 samples IS the 119th sample, so that one frame could be the `sub p99` this sweep
        // reports.
        const bool count_coverage = frame + 1 == warmup;

        render::VirtualGeometryVisibilityRequest request{};
        request.asset = &tree.asset;
        request.asset_id = kAssetId;
        request.residency = &residency;
        request.selection = &cut;
        request.clusters = candidates;
        request.clip_from_object = core::Mat4{}; // identity by default
        request.gpu_selection = &flags;
        request.max_draws = max_draws;

        // VirtualGeometryVisibilityStats is the PASS's running total, not this declare()'s, so a
        // level's candidate count is a difference. Reading it directly reported 220 candidates for
        // a four-leaf quadtree — forty frames of five, which looks like a plausible number and is
        // not one.
        const std::uint32_t offered_before = pass.stats().candidates_offered;
        const std::uint32_t capped_before = pass.stats().skipped_over_candidate_cap;
        pass.declare(graph, ids, depth_bits, depth_target, request);
        auto cmd = device.begin_commands();
        graph.execute(*cmd);
        rhi::BufferHandle id_readback{};
        if (count_coverage) {
            rhi::BufferDesc bd{};
            bd.size = static_cast<std::size_t>(width) * height *
                      sizeof(render::VirtualGeometryVisibilityWords);
            bd.usage = rhi::BufferUsage::TransferDst;
            bd.memory = rhi::MemoryUsage::GpuToCpu;
            id_readback = device.create_buffer(bd);
            cmd->copy_texture_to_buffer(graph.physical(ids), id_readback);
        }
        const rhi::SubmitTicket ticket = device.submit(std::move(cmd));
        rhi::CommandBuffer* done = device.wait_and_borrow(ticket);
        cost.submit_ms = submit_watch.elapsed_ms();

        bool saw_raster = false;
        if (done != nullptr) {
            for (const render::RenderGraph::PassTiming& t : graph.resolve_timings(*done)) {
                cost.passes.push_back(core::PassTiming{std::string(t.name), t.gpu_ms});
                cost.gpu_ms += t.gpu_ms;
                if (t.name.find("vg-visibility") != std::string::npos)
                    saw_raster = true;
            }
        }
        // The guard the bug above earns. A culled pass does not report a time, it reports NO time,
        // and a sum over "whatever came back" cannot tell that from a fast pass. So the frame that
        // did not rasterize is counted rather than averaged into the row.
        if (measuring && !saw_raster)
            ++result.frames_without_raster;
        if (id_readback.is_valid()) {
            std::vector<render::VirtualGeometryVisibilityWords> ids_host(
                static_cast<std::size_t>(width) * height);
            device.read_buffer(id_readback,
                               ids_host.data(),
                               ids_host.size() * sizeof(render::VirtualGeometryVisibilityWords));
            for (const render::VirtualGeometryVisibilityWords& w : ids_host) {
                if ((w.lo | w.hi) != 0u)
                    ++result.covered_pixels;
            }
            device.destroy(id_readback);
        }
        device.release(ticket);
        if (flags.selected_flags.is_valid())
            device.destroy(flags.selected_flags);

        if (!measuring)
            continue;

        result.candidates = pass.stats().candidates_offered - offered_before;
        result.uploaded = static_cast<std::uint32_t>(candidates.size());
        // Differenced for the same reason as the candidate count: this is the only other cumulative
        // stat the ledger publishes, and a running total in a per-level column is the same lie.
        result.skipped_over_candidate_cap = pass.stats().skipped_over_candidate_cap - capped_before;
        result.selected_triangles = triangles_on_cut(tree, cut);
        result.counters = read_counters(device, pass);
        result.stats = pass.stats();
        result.refinement_blocked = cut.refinement_blocked_by_residency;

        const double cpu_ms = cost.select_ms + cost.build_ms + cost.submit_ms;
        cpu.add(cpu_ms);
        select_s.add(cost.select_ms);
        build_s.add(cost.build_ms);
        submit_s.add(cost.submit_ms);
        gpu.add(cost.gpu_ms);
        if (report != nullptr) {
            const auto index = static_cast<std::uint64_t>(frame - warmup);
            report->observe_frame(index, cpu_ms, cost.passes);
            report->observe(std::string(timeline_prefix) + ".select", cost.select_ms);
            report->observe(std::string(timeline_prefix) + ".build", cost.build_ms);
            report->observe(std::string(timeline_prefix) + ".submit", cost.submit_ms);
            report->observe(std::string(timeline_prefix) + ".gpu", cost.gpu_ms);
        }
    }

    result.cpu = cpu.summarize();
    result.select = select_s.summarize();
    result.build = build_s.summarize();
    result.submit = submit_s.summarize();
    result.gpu = gpu.summarize();
    result.measured = cpu.count() > 0;
    return result;
}

} // namespace

int main(int argc, char** argv) {
    bool sweep = false, perf = false;
    int frames = 120, warmup = 8;
    std::uint32_t depth = 3, tris = 64, width = 1280, height = 720;
    const char* out = nullptr;
    const char* baseline = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&](std::uint32_t fallback) -> std::uint32_t {
            return (i + 1 < argc) ? static_cast<std::uint32_t>(std::atoi(argv[++i])) : fallback;
        };
        if (a == "--sweep")
            sweep = true;
        else if (a == "--perf")
            perf = true;
        else if (a == "--frames")
            frames = static_cast<int>(value(static_cast<std::uint32_t>(frames)));
        else if (a == "--warmup")
            warmup = static_cast<int>(value(static_cast<std::uint32_t>(warmup)));
        else if (a == "--depth")
            depth = value(depth);
        else if (a == "--triangles")
            tris = value(tris);
        else if (a == "--width")
            width = value(width);
        else if (a == "--height")
            height = value(height);
        else if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--baseline" && i + 1 < argc)
            baseline = argv[++i];
        else {
            std::fprintf(
                stderr,
                "usage: virtual_geometry --sweep|--perf [--out f.json] [--baseline b.json]\n"
                "                        [--frames N] [--warmup N] [--depth D]\n"
                "                        [--triangles T] [--width W] [--height H]\n");
            return 2;
        }
    }
    if (!sweep && !perf)
        sweep = true;
    // A cluster's triangle index is 7 bits of the visibility ID (v3), so 128 is the ABI ceiling and
    // not a tuning choice — ask for more and the pass would reject every cluster as
    // skipped_too_many_triangles, which reads as "the sweep measured nothing".
    tris = std::clamp(tris, 1u, 128u);
    // At least one warmup frame, because the coverage readback runs on the last one — with none,
    // `covered_pixels` would stay zero and the vacuity guard would fail a run that drew correctly.
    warmup = std::max(warmup, 1);
    // The sweep's rows are depths 1-5, so a depth outside that range would leave the report's
    // `frame` timeline empty and fail the sample-count gate for a reason that looks unrelated.
    if (sweep)
        depth = std::clamp(depth, 1u, 5u);

    std::unique_ptr<rhi::Device> device = rhi::create_device({});
    if (!device) {
        std::fprintf(stderr,
                     "14-virtual-geometry: no Vulkan device — there is nothing to measure\n");
        return 1;
    }
    if (!device->adapter().gpu_driven_draw) {
        std::fprintf(stderr,
                     "14-virtual-geometry: %s reports no gpu_driven_draw; the GPU-built draw path "
                     "this sweep measures cannot run here\n",
                     device->adapter().name.c_str());
        return 1;
    }

    core::MachineFingerprint fp = core::MachineFingerprint::detect();
    const rhi::AdapterInfo& adapter = device->adapter();
    fp.gpu = adapter.name;
    fp.driver = adapter.driver_name + " " + adapter.driver_info;
    fp.width = width;
    fp.height = height;

    std::printf("14-virtual-geometry: %s (%s %s), %ux%u, %u triangles/cluster, "
                "%.1f px/m at %.1f px error\n",
                fp.gpu.c_str(),
                adapter.driver_name.c_str(),
                adapter.driver_info.c_str(),
                width,
                height,
                tris,
                static_cast<double>(kPixelsPerMetre),
                static_cast<double>(kMaxErrorPx));

    core::PerfReport report;
    core::WorkLedger ledger;
    ledger.set("frames.measured", static_cast<std::uint64_t>(frames));
    ledger.set("vg.triangles_per_cluster", tris);

    int status = 0;
    KeyArena keys;
    if (sweep) {
        fp.preset = "gpu-built-draws-sweep";
        report.set_machine(fp);
        report.set_run(core::RunInfo::detect("14-virtual-geometry"));

        // WHAT EACH ROW IS. Depths 1-5 are the scaling series: 4 to 1024 leaves, all of them on
        // the cut, the whole cut fitting the 1024-command capacity. Depth 5 sits exactly AT that
        // capacity rather than past it — the coarse cluster is offered but is not on the cut, so it
        // costs a candidate and not a command, which is a distinction worth stating because the
        // first version of this comment got it wrong and predicted an overflow that cannot happen.
        //
        // Reaching the overflow therefore needs `max_draws`, which exists for exactly this: the
        // final row runs the 64-leaf cut against a 16-command capacity, so the builder must
        // discover the overflow and degrade to the coarse cut. Gate 4 asks for overflow to be
        // counted and to fall back safely, and a sweep that only ever fit would never show either.
        std::printf("\n%14s %7s %7s %11s %10s %8s %8s %8s %8s %8s %8s %6s %9s\n",
                    "level",
                    "groups",
                    "leaves",
                    "candidates",
                    "triangles",
                    "sel p50",
                    "bld p50",
                    "sub p50",
                    "sub p99",
                    "gpu p50",
                    "gpu p99",
                    "drawn",
                    "px cov");
        std::printf("%s\n", std::string(133, '-').c_str());

        struct Row {
            std::uint32_t depth;
            std::uint32_t max_draws; // 0 = the full 1024-command capacity
            const char* label;
        };

        const Row rows[] = {{1, 0, "depth 1"},
                            {2, 0, "depth 2"},
                            {3, 0, "depth 3"},
                            {4, 0, "depth 4"},
                            {5, 0, "depth 5"},
                            {3, 16, "depth 3 cap 16"}};

        for (const Row& row : rows) {
            const std::string_view prefix =
                keys.format("vg.d%u%s", row.depth, row.max_draws != 0 ? "cap" : "");
            const LevelResult r =
                run_level(*device,
                          row.depth,
                          tris,
                          frames,
                          warmup,
                          width,
                          height,
                          (row.max_draws == 0 && row.depth == depth) ? &report : nullptr,
                          std::string(prefix).c_str(),
                          row.max_draws);
            if (!r.measured) {
                status = 1;
                continue;
            }
            // The two candidate counts must agree: one is what the CPU uploaded, the other is what
            // the pass says it offered. They are computed independently, so a mismatch means a gate
            // silently dropped a cluster and the row's "candidates" column is not the cut.
            if (r.frames_without_raster != 0) {
                std::fprintf(stderr,
                             "  %s: %llu of %d measured frames never rasterized — the GPU column "
                             "is a measurement of a culled pass\n",
                             row.label,
                             static_cast<unsigned long long>(r.frames_without_raster),
                             frames);
                status = 1;
            }
            if (r.candidates != r.uploaded) {
                std::fprintf(stderr,
                             "  %s: uploaded %u candidates but the pass offered %u\n",
                             row.label,
                             r.uploaded,
                             r.candidates);
                status = 1;
            }
            std::printf(
                "%14s %7u %7u %11u %10llu %8.3f %8.4f %8.3f %8.3f %8.3f %8.3f %6u %9llu%s\n",
                row.label,
                r.groups,
                r.leaves,
                r.candidates,
                static_cast<unsigned long long>(r.selected_triangles),
                r.select.stat(core::PerfStat::P50),
                r.build.stat(core::PerfStat::P50),
                r.submit.stat(core::PerfStat::P50),
                r.submit.stat(core::PerfStat::P99),
                r.gpu.stat(core::PerfStat::P50),
                r.gpu.stat(core::PerfStat::P99),
                r.counters.emitted,
                static_cast<unsigned long long>(r.covered_pixels),
                r.counters.fell_back_to_coarse != 0 ? "  (coarse fallback)" : "");

            ledger.set(keys.format("%s.candidates", std::string(prefix).c_str()), r.candidates);
            ledger.set(keys.format("%s.selected_triangles", std::string(prefix).c_str()),
                       r.selected_triangles);
            ledger.set(keys.format("%s.emitted_draws", std::string(prefix).c_str()),
                       r.counters.emitted);
            ledger.set(keys.format("%s.selected_total", std::string(prefix).c_str()),
                       r.counters.selected_total);
            ledger.set(keys.format("%s.fell_back_to_coarse", std::string(prefix).c_str()),
                       r.counters.fell_back_to_coarse);
            ledger.set(keys.format("%s.coarse_over_capacity", std::string(prefix).c_str()),
                       r.counters.coarse_over_capacity);
            ledger.set(keys.format("%s.overflow_without_coarse", std::string(prefix).c_str()),
                       r.counters.overflow_without_coarse);
            ledger.set(keys.format("%s.refinement_blocked", std::string(prefix).c_str()),
                       r.refinement_blocked);
            ledger.set(keys.format("%s.skipped_over_candidate_cap", std::string(prefix).c_str()),
                       r.skipped_over_candidate_cap);
            ledger.set(keys.format("%s.covered_pixels", std::string(prefix).c_str()),
                       r.covered_pixels);
            ledger.set(keys.format("%s.frames_without_raster", std::string(prefix).c_str()),
                       r.frames_without_raster);
        }
        std::printf(
            "\n(sel = the selection dispatch's own blocking submit; bld = assembling the "
            "candidate array;\n sub = gate 4's CPU SUBMISSION, i.e. graph declare + record + "
            "submit + wait; gpu = summed\n vg-* pass timestamps. All milliseconds. px cov = "
            "nonzero visibility IDs, out of %u.)\n",
            width * height);
    } else {
        fp.preset = "gpu-built-draws";
        report.set_machine(fp);
        report.set_run(core::RunInfo::detect("14-virtual-geometry"));
        const LevelResult r =
            run_level(*device, depth, tris, frames, warmup, width, height, &report, "vg");
        if (!r.measured) {
            std::fprintf(stderr, "14-virtual-geometry: no frames measured\n");
            return 1;
        }
        std::printf("  depth %u: %u candidates, %llu triangles, %u draws emitted\n"
                    "  cpu submission p50 %.3f p99 %.3f ms · gpu p50 %.3f p99 %.3f ms\n",
                    r.depth,
                    r.candidates,
                    static_cast<unsigned long long>(r.selected_triangles),
                    r.counters.emitted,
                    r.submit.stat(core::PerfStat::P50),
                    r.submit.stat(core::PerfStat::P99),
                    r.gpu.stat(core::PerfStat::P50),
                    r.gpu.stat(core::PerfStat::P99));
        ledger.set("vg.candidates", r.candidates);
        ledger.set("vg.selected_triangles", r.selected_triangles);
        ledger.set("vg.emitted_draws", r.counters.emitted);
        ledger.set("vg.fell_back_to_coarse", r.counters.fell_back_to_coarse);
        ledger.set("vg.overflow_without_coarse", r.counters.overflow_without_coarse);
        ledger.set("vg.refinement_blocked", r.refinement_blocked);
        ledger.set("vg.covered_pixels", r.covered_pixels);
        ledger.set("vg.frames_without_raster", r.frames_without_raster);
    }

    report.set_ledger(ledger);

    // The vacuity guard, and the whole reason the ledger travels with the timings: the cheapest
    // possible run of this sample is one that draws nothing at all. A floor on emitted draws is
    // what makes "fast" and "did the work" inseparable.
    core::WorkBudget budget;
    // In sweep mode depth 3 has exactly 64 leaves, so 64 is an invariant of the generated asset
    // rather than a taste. In --perf mode the depth is the caller's, and a floor of 64 would fail a
    // perfectly valid `--depth 2` run, so there the floor is only what vacuity requires: it drew.
    budget.at_least(sweep ? "vg.d3.emitted_draws" : "vg.emitted_draws", sweep ? 64 : 1)
        .at_least(sweep ? "vg.d3.covered_pixels" : "vg.covered_pixels", 1)
        .at_least("frames.measured", 32);
    for (const core::BudgetViolation& v : budget.check(ledger)) {
        std::fprintf(stderr,
                     "  WORK BUDGET: %.*s = %llu (needs at least %llu)\n",
                     static_cast<int>(v.name.size()),
                     v.name.data(),
                     static_cast<unsigned long long>(v.value),
                     static_cast<unsigned long long>(v.limit));
        status = 1;
    }

    // The vacuity guard, and it is the whole reason the ledger travels with the timings: the
    // cheapest possible run of this sample is one that draws nothing at all. A floor on emitted
    // draws is what makes "fast" and "did the work" inseparable.
    // There is no ratified ceiling for virtual geometry — gate 4 asks for the sweep to REPORT the
    // four columns, not to pass a budget, and inventing a limit here would be a number with no
    // authority behind it. So the gate is only what a report can honestly assert about itself: the
    // run was long enough to have a tail, and it did not slide against the committed baseline.
    core::PerfGate gate;
    gate.require_samples("frame", 32).max_regression(0.10);

    core::PerfReport loaded;
    const core::PerfReport* baseline_ptr = nullptr;
    if (baseline != nullptr) {
        std::string error;
        if (core::PerfReport::load_file(baseline, loaded, error))
            baseline_ptr = &loaded;
        else
            std::printf("  (no usable baseline: %s)\n", error.c_str());
    }
    const core::PerfGate::Result result = gate.check(report, baseline_ptr);
    if (!result.ok())
        status = 1;

    std::printf("  work ledger: %s\n", ledger.to_json(-1).c_str());
    if (!result.ok())
        std::fprintf(stderr, "  PERF GATE:\n%s", core::PerfGate::format(result).c_str());
    else
        std::printf("  perf gate: %s", core::PerfGate::format(result).c_str());

    if (out != nullptr) {
        FILE* f = std::fopen(out, "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "14-virtual-geometry: could not write %s\n", out);
            status = 1;
        } else {
            const std::string json = report.to_json();
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
            std::printf("  wrote %s\n", out);
        }
    }
    device->wait_idle();
    return status;
}
