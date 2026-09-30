// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M18.4: the software micro-triangle raster pass. The technique lives in vg_micro_raster.comp and
// the header; the notes here are about pass wiring — which graph edges order what, and why the
// pipeline set depends on the adapter.

#include "rime/render/virtual_geometry_micro_raster_pass.hpp"

#include <algorithm>
#include <cmath>

#include "fullscreen.vert.spv.h"
#include "rime/render/passes.hpp"
#include "vg_micro_clear.comp.spv.h"
#include "vg_micro_merge.frag.spv.h"
#include "vg_micro_raster.comp.spv.h"
#include "vg_micro_raster_depth32.comp.spv.h"
#include "vg_micro_raster_id32.comp.spv.h"

namespace rime::render {

namespace {

constexpr std::uint32_t kGroupSize = 64;         // local_size_x of both compute shaders
constexpr std::uint32_t kMaxClearGroups = 65535; // Vulkan's guaranteed maxComputeWorkGroupCount

// Mirrors the push block of vg_micro_raster.comp.
struct RasterPush {
    std::uint32_t triangle_count = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t flags = 0;
    float max_extent_px = 0.0f;
};

static_assert(sizeof(RasterPush) == 20, "RasterPush must match vg_micro_raster.comp");

constexpr rhi::BindingDesc kClearBindings[] = {
    {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
};
constexpr rhi::BindingDesc kRasterBindings[] = {
    {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // triangles
    {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // samples
    {2, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // counters
};
constexpr rhi::BindingDesc kMergeBindings[] = {
    {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment}, // triangles
    {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment}, // samples
};
constexpr rhi::Format kTargetFormats[] = {rhi::Format::RG32Uint, rhi::Format::R32Uint};

rhi::ShaderHandle make_shader(rhi::Device& device,
                              rhi::ShaderStage stage,
                              const std::uint32_t* spirv,
                              std::size_t bytes,
                              std::string_view name) {
    rhi::ShaderDesc sd{};
    sd.stage = stage;
    sd.spirv = spirv;
    sd.spirv_size_bytes = bytes;
    sd.debug_name = name;
    return device.create_shader(sd);
}

rhi::PipelineHandle make_compute(rhi::Device& device,
                                 rhi::ShaderHandle shader,
                                 std::span<const rhi::BindingDesc> bindings,
                                 std::uint32_t push_size,
                                 std::string_view name) {
    rhi::ComputePipelineDesc cd{};
    cd.shader = shader;
    cd.bindings = bindings;
    cd.push_constant_size = push_size;
    cd.debug_name = name;
    return device.create_compute_pipeline(cd);
}

} // namespace

VirtualGeometryMicroRasterPass::VirtualGeometryMicroRasterPass(rhi::Device& device)
    : device_(device) {
    clear_shader_ = make_shader(device,
                                rhi::ShaderStage::Compute,
                                vg_micro_clear_comp_spv,
                                sizeof(vg_micro_clear_comp_spv),
                                "vg_micro_clear.comp");
    clear_pipeline_ = make_compute(
        device, clear_shader_, kClearBindings, sizeof(std::uint32_t), "vg-micro-clear");

    // The 64-bit module declares the Int64Atomics capability; creating it on a device without the
    // feature is invalid usage, not merely slow — so it is only ever built where it can run.
    if (device.adapter().buffer_int64_atomics) {
        raster64_shader_ = make_shader(device,
                                       rhi::ShaderStage::Compute,
                                       vg_micro_raster_comp_spv,
                                       sizeof(vg_micro_raster_comp_spv),
                                       "vg_micro_raster.comp");
        raster64_pipeline_ = make_compute(
            device, raster64_shader_, kRasterBindings, sizeof(RasterPush), "vg-micro-raster");
    }
    depth32_shader_ = make_shader(device,
                                  rhi::ShaderStage::Compute,
                                  vg_micro_raster_depth32_comp_spv,
                                  sizeof(vg_micro_raster_depth32_comp_spv),
                                  "vg_micro_raster_depth32.comp");
    depth32_pipeline_ = make_compute(
        device, depth32_shader_, kRasterBindings, sizeof(RasterPush), "vg-micro-raster-depth");
    id32_shader_ = make_shader(device,
                               rhi::ShaderStage::Compute,
                               vg_micro_raster_id32_comp_spv,
                               sizeof(vg_micro_raster_id32_comp_spv),
                               "vg_micro_raster_id32.comp");
    id32_pipeline_ = make_compute(
        device, id32_shader_, kRasterBindings, sizeof(RasterPush), "vg-micro-raster-id");

    merge_vs_ = make_shader(device,
                            rhi::ShaderStage::Vertex,
                            fullscreen_vert_spv,
                            sizeof(fullscreen_vert_spv),
                            "fullscreen.vert");
    merge_fs_ = make_shader(device,
                            rhi::ShaderStage::Fragment,
                            vg_micro_merge_frag_spv,
                            sizeof(vg_micro_merge_frag_spv),
                            "vg_micro_merge.frag");
    // The merge's depth state IS the hardware/software arbitration (see vg_micro_merge.frag):
    // Less, so an exact tie keeps the hardware pixel, and a write, so the depth attachment stays
    // the true nearest depth for whatever reads it next. No cull: it is one fullscreen triangle.
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = merge_vs_;
    pd.fragment_shader = merge_fs_;
    pd.color_formats = kTargetFormats;
    pd.cull = rhi::CullMode::None;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = kDepthFormat;
    pd.bindings = kMergeBindings;
    pd.push_constant_size = sizeof(std::uint32_t);
    pd.debug_name = "vg-micro-merge";
    merge_pipeline_ = device.create_graphics_pipeline(pd);
}

VirtualGeometryMicroRasterPass::~VirtualGeometryMicroRasterPass() {
    release_buffers();
    for (rhi::PipelineHandle* p : {&merge_pipeline_,
                                   &id32_pipeline_,
                                   &depth32_pipeline_,
                                   &raster64_pipeline_,
                                   &clear_pipeline_}) {
        if (p->is_valid()) {
            device_.destroy(*p);
        }
    }
    for (rhi::ShaderHandle* s : {&merge_fs_,
                                 &merge_vs_,
                                 &id32_shader_,
                                 &depth32_shader_,
                                 &raster64_shader_,
                                 &clear_shader_}) {
        if (s->is_valid()) {
            device_.destroy(*s);
        }
    }
}

void VirtualGeometryMicroRasterPass::release_buffers() noexcept {
    for (rhi::BufferHandle* b : {&triangles_, &counters_}) {
        if (b->is_valid()) {
            device_.destroy(*b);
        }
        *b = {};
    }
}

bool VirtualGeometryMicroRasterPass::declare(RenderGraph& graph,
                                             RGTexture visibility,
                                             RGTexture depth_bits,
                                             RGTexture depth,
                                             const VirtualGeometryMicroRasterRequest& request) {
    release_buffers();
    stats_ = {};
    stats_.offered =
        static_cast<std::uint32_t>(std::min<std::size_t>(request.triangles.size(), UINT32_MAX));
    if (request.triangles.empty()) {
        return false; // nothing to add: the hardware result stands as it is
    }
    // A request the shaders cannot address is refused whole, and counted as such — never
    // rasterized against a guessed size. max_extent_px must be a usable, finite cap.
    const bool size_ok =
        request.width != 0 && request.height != 0 &&
        std::uint64_t{request.width} * request.height <= (1ull << 30); // 2 words/pixel in u32
    if (!size_ok || !std::isfinite(request.max_extent_px) || request.max_extent_px < 0.0f) {
        stats_.skipped_bad_request = stats_.offered;
        return false;
    }

    const auto uploaded = static_cast<std::uint32_t>(
        std::min<std::size_t>(request.triangles.size(), kVirtualGeometryMaxMicroTriangles));
    stats_.uploaded = uploaded;
    stats_.skipped_over_capacity = stats_.offered - uploaded;
    const bool use64 = request.atomics == MicroRasterAtomics::Auto && raster64_pipeline_.is_valid();
    stats_.used_int64_atomics = use64 ? 1u : 0u;

    rhi::BufferDesc td{};
    td.size = std::size_t{uploaded} * sizeof(VirtualGeometryMicroTriangle);
    td.usage = rhi::BufferUsage::Storage;
    td.memory = rhi::MemoryUsage::CpuToGpu;
    td.initial_data = request.triangles.data();
    td.debug_name = "vg-micro-triangles";
    triangles_ = device_.create_buffer(td);

    // GpuToCpu so a test can read what the rasterizer saw; the frame path never does.
    const VirtualGeometryMicroRasterCounters zero{};
    rhi::BufferDesc cd{};
    cd.size = sizeof(zero);
    cd.usage = rhi::BufferUsage::Storage;
    cd.memory = rhi::MemoryUsage::GpuToCpu;
    cd.initial_data = &zero;
    cd.debug_name = "vg-micro-counters";
    counters_ = device_.create_buffer(cd);

    const std::uint32_t pixel_count = request.width * request.height;
    const RGBuffer samples_rg = graph.create_buffer(
        {std::uint64_t{pixel_count} * 2u * sizeof(std::uint32_t), "vg-micro-samples"});
    const RGBuffer triangles_rg = graph.import_buffer(triangles_, rhi::ResourceState::ShaderRead);
    const RGBuffer counters_rg = graph.import_buffer(counters_, rhi::ResourceState::ShaderRead);
    graph.export_buffer(counters_rg);

    // 1. Clear. A write, so the raster pass that follows is ordered after it by the graph's
    //    write->write buffer barrier (render_graph.cpp).
    {
        const RGBuffer writes[] = {samples_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.buffer_writes = writes;
        const std::uint32_t words = pixel_count * 2u;
        const std::uint32_t groups =
            std::min(kMaxClearGroups, (words + kGroupSize - 1u) / kGroupSize);
        graph.add_compute_pass(
            "vg-micro-clear",
            desc,
            [pipe = clear_pipeline_, &graph, samples_rg, words, groups](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_buffer(0, graph.physical_buffer(samples_rg));
                cmd.push_constants(&words, sizeof(words));
                cmd.dispatch(groups, 1, 1);
            });
    }

    // 2. Raster: one pass with 64-bit atomics, else the two-pass 32-bit twin. Each pass WRITES the
    //    sample buffer, so the graph puts a barrier between the depth pass and the id pass — the id
    //    pass reads the settled depth key, and a missing barrier would be a race, not a slowdown.
    RasterPush push{};
    push.triangle_count = uploaded;
    push.width = request.width;
    push.height = request.height;
    push.flags = request.cull_back_faces ? 1u : 0u;
    push.max_extent_px = request.max_extent_px;
    const std::uint32_t raster_groups = (uploaded + kGroupSize - 1u) / kGroupSize;
    const auto add_raster = [&](std::string_view name, rhi::PipelineHandle pipe) {
        const RGBuffer reads[] = {triangles_rg};
        const RGBuffer writes[] = {samples_rg, counters_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.buffer_reads = reads;
        desc.buffer_writes = writes;
        graph.add_compute_pass(
            name,
            desc,
            [pipe, &graph, triangles_rg, samples_rg, counters_rg, push, raster_groups](
                rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_buffer(0, graph.physical_buffer(triangles_rg));
                cmd.bind_storage_buffer(1, graph.physical_buffer(samples_rg));
                cmd.bind_storage_buffer(2, graph.physical_buffer(counters_rg));
                cmd.push_constants(&push, sizeof(push));
                cmd.dispatch(raster_groups, 1, 1);
            });
    };
    if (use64) {
        add_raster("vg-micro-raster", raster64_pipeline_);
    } else {
        add_raster("vg-micro-raster-depth", depth32_pipeline_);
        add_raster("vg-micro-raster-id", id32_pipeline_);
    }

    // 3. Merge into the hardware pass's targets: Load everything, let the depth test arbitrate.
    const RGColorAttachment colors[] = {
        {visibility, rhi::LoadOp::Load, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}},
        {depth_bits, rhi::LoadOp::Load, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const RGDepthAttachment depth_att{
        depth, rhi::LoadOp::Load, rhi::StoreOp::Store, 1.0f, 0, false};
    const RGBuffer merge_reads[] = {triangles_rg, samples_rg};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;
    desc.buffer_reads = merge_reads;
    const std::uint32_t width = request.width;
    graph.add_raster_pass(
        "vg-micro-merge",
        desc,
        [pipe = merge_pipeline_, &graph, triangles_rg, samples_rg, width](rhi::CommandBuffer& cmd) {
            cmd.bind_pipeline(pipe);
            cmd.bind_storage_buffer(0, graph.physical_buffer(triangles_rg));
            cmd.bind_storage_buffer(1, graph.physical_buffer(samples_rg));
            cmd.push_constants(&width, sizeof(width));
            cmd.draw(3);
        });
    return true;
}

} // namespace rime::render
