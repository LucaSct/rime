// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"

// Clustered forward shading (m10.3, ADR-0032 §4): many lights, at the cost of the ones that
// actually reach each pixel.
//
// The problem it solves. M5.6's forward shader loops a fixed uniform block of ≤16 point lights for
// EVERY pixel (ADR-0022) — so 200 lights is both impossible (the block is statically sized) and
// pointless (a pixel is touched by a handful of them; the rest are 200 wasted BRDF evaluations).
//
// The idea. Dice the camera frustum into a 3-D grid of FROXELS — frustum voxels, 16×9 across the
// screen × 24 slices in depth (settings.hpp) — and once per frame, in compute, work out which
// lights touch which froxel. Shading then reads only its own froxel's short list. Culling costs
// clusters × lights sphere/AABB tests once; shading saves lights × pixels BRDF evaluations. That
// trade is why every modern forward renderer is clustered.
//
// The two halves have to agree exactly on where a froxel is, so this header exposes the CPU
// spelling of the froxel math (cluster_depth_slice / cluster_bounds / sphere_touches_cluster) and
// cluster_cull.comp mirrors it line for line. That is deliberate: the mirror is unit-testable
// without a GPU, and the GPU proof asserts the two agree cluster-for-cluster over the whole grid
// — a much sharper instrument than looking at pixels and hoping.
//
// The log-z depth partition, the froxel AABB derivation, and the sphere test are in
// docs/math/clustered-shading.md.
namespace rime::render {

// What the froxel grid needs to know about the view it tiles. `view` is world → view; the
// projection is described by the same four numbers the renderer builds it from, so this struct can
// never disagree with core::perspective (which it re-derives, rather than being handed a matrix to
// invert).
struct ClusterInputs {
    core::Mat4 view;
    float fov_y = 0.87266f;
    float aspect = 1.0f;
    float z_near = 0.1f;
    float z_far = 1000.0f;
    rhi::Extent2D extent{}; // the render target the screen tiles cover
};

// A froxel's bounds in VIEW space (camera at the origin looking down −z). Conservative: the true
// froxel is a frustum slab and this is its axis-aligned box, so the sphere test can only ever say
// "touching" for a light that in truth just misses — never the reverse. Over-inclusion costs a few
// wasted BRDF evaluations; under-inclusion would be a missing light, i.e. a visible bug.
struct ClusterBounds {
    core::Vec3 min{0.0f, 0.0f, 0.0f};
    core::Vec3 max{0.0f, 0.0f, 0.0f};
};

// Which depth slice a view-space distance falls in, under the LOGARITHMIC partition
// (docs/math/clustered-shading.md §2): slice k spans [near·(far/near)^(k/K),
// near·(far/near)^((k+1)/K)). Log-z gives every slice the same *relative* depth range, which is
// what keeps froxels roughly cube-shaped from the near plane to the horizon — a uniform partition
// would make the first slice a sliver and the last one a mile deep. Clamped to [0, kClusterGridZ-1]
// so nothing outside the frustum can index out of bounds.
[[nodiscard]] std::uint32_t cluster_depth_slice(float view_depth, float z_near, float z_far);

// The flat index of froxel (x, y, slice) — slice-major so one depth slice is contiguous, which is
// how the cull dispatch walks them.
[[nodiscard]] inline std::uint32_t
cluster_index(std::uint32_t x, std::uint32_t y, std::uint32_t slice) noexcept {
    return (slice * kClusterGridY + y) * kClusterGridX + x;
}

// The view-space AABB of one froxel (0 ≤ cluster < kClusterCount).
[[nodiscard]] ClusterBounds cluster_bounds(std::uint32_t cluster, const ClusterInputs& in);

// Does a light sphere of radius `radius` centred at `center_view` (VIEW space) touch this froxel?
// The standard point-to-AABB test: clamp the centre into the box, measure what is left.
[[nodiscard]] bool
sphere_touches_cluster(const ClusterBounds& bounds, core::Vec3 center_view, float radius);

// ── GPU mirrors ───────────────────────────────────────────────────────────────────────────────

// std140 twin of the ClusterUniforms block shared by cluster_cull.comp and the forward shader.
// Every member is a mat4 or a 16-byte vec4/uvec4, so C++'s layout and std140 agree by construction
// (the same discipline as GpuFrameUniforms).
struct GpuClusterUniforms {
    core::Mat4 view;                                // world → view (culling happens here)
    std::uint32_t grid[4] = {kClusterGridX,         //
                             kClusterGridY,         //
                             kClusterGridZ,         //
                             kMaxLightsPerCluster}; //
    std::uint32_t counts[4] = {0, 0, 0, 0};         // x = light count, y = clustered on/off
    float depth[4] = {0.1f, 1000.0f, 0.0f, 0.0f};   // x = z_near, y = z_far, z = log(far/near)
    float screen[4] = {1.0f, 1.0f, 0.0f, 0.0f};     // x,y = target size in pixels
    float proj[4] = {1.0f, -1.0f, 0.0f, 0.0f};      // x,y = the projection's x/y scales
};

static_assert(sizeof(GpuClusterUniforms) == 144 && offsetof(GpuClusterUniforms, grid) == 64 &&
                  offsetof(GpuClusterUniforms, counts) == 80 &&
                  offsetof(GpuClusterUniforms, depth) == 96 &&
                  offsetof(GpuClusterUniforms, screen) == 112 &&
                  offsetof(GpuClusterUniforms, proj) == 128,
              "GpuClusterUniforms no longer matches the std140 ClusterUniforms block");

// Bytes in the per-froxel list buffer. Each froxel owns a contiguous run of (1 + max) uints: the
// count first, then that many light indices. One buffer, one binding, and a froxel's count sits in
// the same cache line as the indices that follow it.
inline constexpr std::uint32_t kClusterListStride = 1 + kMaxLightsPerCluster;
inline constexpr std::uint64_t kClusterListBytes =
    static_cast<std::uint64_t>(kClusterCount) * kClusterListStride * sizeof(std::uint32_t);

// ── The pass ──────────────────────────────────────────────────────────────────────────────────

// Owns the clustered-lighting GPU resources — the cull pipeline, the packed light buffer (grown to
// fit the scene), the uniform block both halves read, and the placeholder buffers the off path
// binds — and declares the cull dispatch each frame. One per SceneRenderer.
class ClusteredLights {
public:
    explicit ClusteredLights(rhi::Device& device);
    ~ClusteredLights();

    ClusteredLights(const ClusteredLights&) = delete;
    ClusteredLights& operator=(const ClusteredLights&) = delete;

    // Upload `lights` (all of them — no cap), declare the cull dispatch into `graph`, and return
    // what the forward pass binds. The list buffer is a graph TRANSIENT: it is rebuilt from
    // scratch every frame, so there is nothing to cache and nothing to invalidate (contrast the
    // local-shadow maps, whose whole design is the cache).
    [[nodiscard]] ClusterBinding
    add(RenderGraph& graph, std::span<const GpuPointLight> lights, const ClusterInputs& inputs);

    // The valid-but-empty binding for a frame that shades WITHOUT clustering (clustered off, or on
    // with no point lights): the shadowed pipeline's bindings 11–13 must still point at real
    // buffers, and the uniform block's `enabled` flag being 0 is what sends the shader down the
    // uniform-block loop instead.
    [[nodiscard]] ClusterBinding empty_binding(RenderGraph& graph);

    // How many lights the last add() culled — what the perf note quotes alongside the pass timings.
    [[nodiscard]] std::uint32_t last_light_count() const noexcept { return last_light_count_; }

private:
    void ensure_light_capacity(std::uint32_t count);

    rhi::Device& device_;
    rhi::ShaderHandle cull_shader_;
    rhi::PipelineHandle cull_pipeline_;
    rhi::BufferHandle light_buffer_; // packed GpuPointLight array, grown on demand
    std::uint32_t light_capacity_ = 0;
    rhi::BufferHandle uniforms_;    // GpuClusterUniforms
    rhi::BufferHandle empty_lists_; // a one-froxel, count-0 list buffer for the off path
    std::uint32_t last_light_count_ = 0;
};

} // namespace rime::render
