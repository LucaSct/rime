// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Clustered forward shading, CPU half (m10.3, ADR-0032 §4): the froxel math that
// cluster_cull.comp mirrors, plus the resources and the one dispatch that fills the per-froxel
// light lists. Derivation: docs/math/clustered-shading.md.

#include "rime/render/lighting/clustered.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "cluster_cull.comp.spv.h"
#include "rime/core/diagnostics/log.hpp"

namespace rime::render {

namespace {

// The cull dispatch's workgroup size — must equal cluster_cull.comp's local_size_x (which also
// sizes its shared-memory light batch).
constexpr std::uint32_t kCullGroupSize = 64;

// The initial light-buffer capacity. Big enough that ordinary scenes never reallocate, small
// enough (2 KB) to be free.
constexpr std::uint32_t kInitialLightCapacity = 64;

// The projection's x/y scale terms, re-derived from the lens exactly as core::perspective builds
// them: p00 = 1/(aspect·tan(fov/2)), p11 = −1/tan(fov/2). p11 is negative because our clip space
// is y-DOWN (Vulkan). Culling inverts these to walk from NDC back to view space, so they must be
// the *same* two numbers the renderer projected with — hence re-derivation from the lens rather
// than digging them out of a matrix.
struct ProjScale {
    float p00;
    float p11;
};

ProjScale proj_scale(float fov_y, float aspect) {
    const float tan_half = std::tan(fov_y * 0.5f);
    return {1.0f / (std::max(aspect, 1e-4f) * tan_half), -1.0f / tan_half};
}

} // namespace

std::uint32_t cluster_depth_slice(float view_depth, float z_near, float z_far) {
    // slice = ⌊K · log(d/near) / log(far/near)⌋ — the inverse of the partition
    // d_k = near·(far/near)^(k/K). Anything nearer than the near plane lands in slice 0; anything
    // past the far plane in the last slice (a fragment beyond z_far was going to be clipped
    // anyway, and clamping is what keeps the index in bounds).
    const float near = std::max(z_near, 1e-4f);
    const float ratio = std::max(z_far / near, 1.0f + 1e-6f);
    const float t = std::log(std::max(view_depth, near) / near) / std::log(ratio);
    const auto slice = static_cast<std::int32_t>(std::floor(t * static_cast<float>(kClusterGridZ)));
    return static_cast<std::uint32_t>(
        std::clamp(slice, 0, static_cast<std::int32_t>(kClusterGridZ) - 1));
}

ClusterBounds cluster_bounds(std::uint32_t cluster, const ClusterInputs& in) {
    const std::uint32_t x = cluster % kClusterGridX;
    const std::uint32_t y = (cluster / kClusterGridX) % kClusterGridY;
    const std::uint32_t z = cluster / (kClusterGridX * kClusterGridY);

    const float ndc_x0 = static_cast<float>(x) / kClusterGridX * 2.0f - 1.0f;
    const float ndc_x1 = static_cast<float>(x + 1) / kClusterGridX * 2.0f - 1.0f;
    const float ndc_y0 = static_cast<float>(y) / kClusterGridY * 2.0f - 1.0f;
    const float ndc_y1 = static_cast<float>(y + 1) / kClusterGridY * 2.0f - 1.0f;

    const float near = std::max(in.z_near, 1e-4f);
    const float log_ratio = std::log(std::max(in.z_far / near, 1.0f + 1e-6f));
    const float d0 = near * std::exp(static_cast<float>(z) / kClusterGridZ * log_ratio);
    const float d1 = near * std::exp(static_cast<float>(z + 1) / kClusterGridZ * log_ratio);

    const ProjScale ps = proj_scale(in.fov_y, in.aspect);
    ClusterBounds b;
    b.min = core::Vec3{1e30f, 1e30f, 1e30f};
    b.max = core::Vec3{-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) {
        const float nx = (i & 1) == 0 ? ndc_x0 : ndc_x1;
        const float ny = (i & 2) == 0 ? ndc_y0 : ndc_y1;
        const float d = (i & 4) == 0 ? d0 : d1;
        const core::Vec3 p{nx * d / ps.p00, ny * d / ps.p11, -d};
        b.min = core::Vec3{std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
        b.max = core::Vec3{std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
    }
    return b;
}

bool sphere_touches_cluster(const ClusterBounds& bounds, core::Vec3 center_view, float radius) {
    const core::Vec3 closest{std::clamp(center_view.x, bounds.min.x, bounds.max.x),
                             std::clamp(center_view.y, bounds.min.y, bounds.max.y),
                             std::clamp(center_view.z, bounds.min.z, bounds.max.z)};
    const core::Vec3 d = center_view - closest;
    return core::dot(d, d) <= radius * radius;
}

// ── ClusteredLights ───────────────────────────────────────────────────────────────────────────

ClusteredLights::ClusteredLights(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc sd{};
    sd.stage = rhi::ShaderStage::Compute;
    sd.spirv = cluster_cull_comp_spv;
    sd.spirv_size_bytes = sizeof(cluster_cull_comp_spv);
    sd.debug_name = "cluster_cull.comp";
    cull_shader_ = device.create_shader(sd);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // the packed lights
        {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // the per-froxel lists
        {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute}, // the grid description
    };
    rhi::ComputePipelineDesc pd{};
    pd.shader = cull_shader_;
    pd.bindings = bindings;
    pd.debug_name = "cluster-cull";
    cull_pipeline_ = device.create_compute_pipeline(pd);

    rhi::BufferDesc ub{};
    ub.size = sizeof(GpuClusterUniforms);
    ub.usage = rhi::BufferUsage::Uniform;
    ub.memory = rhi::MemoryUsage::CpuToGpu;
    ub.debug_name = "cluster-uniforms";
    uniforms_ = device.create_buffer(ub);

    // The off-path placeholder: one froxel's worth of list storage so binding 12 always points at
    // a real buffer. Never read (the shader's `enabled` flag gates it), never written.
    rhi::BufferDesc eb{};
    eb.size = kClusterListStride * sizeof(std::uint32_t);
    eb.usage = rhi::BufferUsage::Storage;
    eb.memory = rhi::MemoryUsage::GpuOnly;
    eb.debug_name = "cluster-lists-empty";
    empty_lists_ = device.create_buffer(eb);

    ensure_light_capacity(kInitialLightCapacity);
}

ClusteredLights::~ClusteredLights() {
    device_.destroy(empty_lists_);
    device_.destroy(uniforms_);
    if (light_buffer_.is_valid())
        device_.destroy(light_buffer_);
    device_.destroy(cull_pipeline_);
    device_.destroy(cull_shader_);
}

void ClusteredLights::ensure_light_capacity(std::uint32_t count) {
    if (count <= light_capacity_)
        return;
    // Grow geometrically so a scene ramping its light count up does not reallocate every frame
    // (the ensure_draw_capacity pattern from the scene renderer).
    std::uint32_t capacity = std::max(light_capacity_, kInitialLightCapacity);
    while (capacity < count)
        capacity *= 2;
    if (light_buffer_.is_valid())
        device_.destroy(light_buffer_);
    rhi::BufferDesc bd{};
    bd.size = static_cast<std::uint64_t>(capacity) * sizeof(GpuPointLight);
    bd.usage = rhi::BufferUsage::Storage;
    bd.memory = rhi::MemoryUsage::CpuToGpu; // the CPU packs it every frame
    bd.debug_name = "cluster-lights";
    light_buffer_ = device_.create_buffer(bd);
    light_capacity_ = capacity;
}

ClusterBinding ClusteredLights::add(RenderGraph& graph,
                                    std::span<const GpuPointLight> lights,
                                    const ClusterInputs& inputs) {
    const auto light_count = static_cast<std::uint32_t>(lights.size());
    last_light_count_ = light_count;
    ensure_light_capacity(std::max(light_count, 1u));
    if (light_count > 0) {
        device_.write_buffer(light_buffer_,
                             lights.data(),
                             static_cast<std::size_t>(light_count) * sizeof(GpuPointLight));
    }

    GpuClusterUniforms cu{};
    cu.view = inputs.view;
    cu.counts[0] = light_count;
    cu.counts[1] = 1; // clustered ON — the flag the forward shader branches on
    const float near = std::max(inputs.z_near, 1e-4f);
    cu.depth[0] = near;
    cu.depth[1] = inputs.z_far;
    cu.depth[2] = std::log(std::max(inputs.z_far / near, 1.0f + 1e-6f));
    cu.screen[0] = static_cast<float>(inputs.extent.width);
    cu.screen[1] = static_cast<float>(inputs.extent.height);
    const ProjScale ps = proj_scale(inputs.fov_y, inputs.aspect);
    cu.proj[0] = ps.p00;
    cu.proj[1] = ps.p11;
    device_.write_buffer(uniforms_, &cu, sizeof(cu));

    // The light array is host-written and persistent, so it enters the graph as an IMPORT already
    // in ShaderRead (Vulkan's submission guarantee covers the host write). The lists are a pure
    // per-frame product: a transient, rebuilt from scratch by the dispatch below.
    const RGBuffer light_res = graph.import_buffer(light_buffer_, rhi::ResourceState::ShaderRead);
    const RGBuffer lists = graph.create_buffer({kClusterListBytes, "cluster-lists"});

    const RGBuffer reads[] = {light_res};
    const RGBuffer writes[] = {lists};
    RenderGraph::ComputePassDesc desc{};
    desc.buffer_reads = reads;
    desc.buffer_writes = writes;
    graph.add_compute_pass("cluster-cull",
                           desc,
                           [pipe = cull_pipeline_, ubo = uniforms_, light_res, lists, &graph](
                               rhi::CommandBuffer& cmd) {
                               cmd.bind_compute_pipeline(pipe);
                               cmd.bind_storage_buffer(0, graph.physical_buffer(light_res));
                               cmd.bind_storage_buffer(1, graph.physical_buffer(lists));
                               cmd.bind_uniform_buffer(2, ubo);
                               // One invocation per froxel, rounded up to whole workgroups; the
                               // tail invocations run the same code and simply never write (see the
                               // shader's barrier note).
                               cmd.dispatch(
                                   (kClusterCount + kCullGroupSize - 1) / kCullGroupSize, 1, 1);
                           });

    return {light_res, lists, uniforms_};
}

ClusterBinding ClusteredLights::empty_binding(RenderGraph& graph) {
    last_light_count_ = 0;
    // Everything zero but the grid dims — counts[1] (the enabled flag) staying 0 is what sends the
    // forward shader down the M5.6 uniform-block light loop.
    GpuClusterUniforms cu{};
    device_.write_buffer(uniforms_, &cu, sizeof(cu));
    return {graph.import_buffer(light_buffer_, rhi::ResourceState::ShaderRead),
            graph.import_buffer(empty_lists_, rhi::ResourceState::ShaderRead),
            uniforms_};
}

} // namespace rime::render
