// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/virtual_geometry.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_selection.hpp"

// M18 step 1: the first hardware **visibility buffer** pass for virtual geometry.
//
// The technique (a "visibility buffer", Burns & Hunt 2013, the idea Nanite builds on): instead of
// shading while rasterizing, write only *which triangle* covers each pixel into an integer target,
// and let a later pass fetch attributes and shade exactly once per pixel. Here the integer is the
// packed 64-bit RG32Uint visibility-ID ABI (virtual_geometry_visibility_id.hpp, version 3) —
// triangle, cluster slot, generation and format version — so a recycled slot cannot be confused
// with an old pixel and the resolve pass (virtual_geometry_resolve_pass.hpp) can find the triangle.
//
// **Indexed-indirect submission** (M18.3b). The pass submits all accepted clusters as one
// `draw_indexed_indirect` with a fixed-capacity command buffer. Per-cluster values live in a
// std430 storage buffer indexed by `gl_DrawIDARB`; the MVP alone remains a push constant. An
// identity index buffer is bound so `gl_VertexIndex` is the global cluster index, letting the
// vertex shader keep pulling from the existing storage buffers and derive the triangle as
// `(gl_VertexIndex - index_base) / 3`. This avoids `gl_PrimitiveID`, which needs the Vulkan
// `geometryShader` feature MoltenVK does not have, and keeps the resolve pass reading the same
// buffers this pass drew from.
//
// The fragment stage ALSO writes gl_FragCoord.z's bits to a second R32Uint target: the RHI's
// depth readback copies the colour aspect only, so encoding depth into a copyable integer is how
// a test (and a future debug view) can inspect it without widening the RHI.
//
// Stub/limits: every declare() uploads the requested clusters from their page bytes through
// view_virtual_geometry_page() into host-visible storage buffers owned by this pass. There is no
// GPU page pool, no instancing, and no cluster culling yet — those are later M18 steps.
namespace rime::render {

// One cluster to draw. The residency slot and allocation generation are what the upload
// scheduler would assign; until that scheduler exists the caller supplies them, and they are
// packed into the pixel verbatim (version-3 bounds: slot < 2^25, generation < 2^28).
struct VirtualGeometryClusterDraw {
    std::uint32_t cluster = assets::kInvalidVirtualGeometryIndex;
    std::uint32_t cluster_slot = 0;
    std::uint32_t generation = 0;
};

struct VirtualGeometryVisibilityRequest {
    const assets::VirtualGeometryAsset* asset = nullptr;
    assets::AssetId asset_id{};
    const VirtualGeometryResidency* residency = nullptr;
    // The cut produced by select_virtual_geometry(); each cluster must belong to one of these
    // groups, and that group must be a leaf (no children) — this pass draws leaf clusters only.
    const VirtualGeometrySelection* selection = nullptr;
    // Every cluster is gated independently: a rejected one is counted and simply not drawn.
    std::span<const VirtualGeometryClusterDraw> clusters = {};
    core::Mat4 clip_from_object{};
};

// The GPU-side cluster data a declare() uploaded, for the resolve pass to read. All three are
// host-visible storage buffers (std430):
//   vertices — the drawn clusters' interleaved cooked vertices, back to back, as u32 words;
//   indices  — their cluster-local u32 indices, back to back;
//   clusters — one VirtualGeometryGpuCluster per slot in [0, cluster_count), indexed BY SLOT so
//              a pixel's slot finds its record in one fetch. Unused slots have `valid == 0`.
// Handles are invalid when nothing was drawn, and all are replaced by the next declare().
struct VirtualGeometryGpuCluster {
    std::uint32_t vertex_base = 0;    // first vertex of this cluster in `vertices`
    std::uint32_t index_base = 0;     // first index of this cluster in `indices`
    std::uint32_t triangle_count = 0; // index_count / 3
    std::uint32_t material_slot = 0;  // VirtualGeometryCluster::material_slot
    std::uint32_t generation = 0;     // must match the pixel's, or the pixel is stale
    std::uint32_t valid = 0;
    std::uint32_t pad[2] = {};
};

static_assert(sizeof(VirtualGeometryGpuCluster) == 32, "must match the shaders' std430 struct");

// Per-draw data for the single indexed-indirect submission. Push constants cannot vary between
// commands, so the per-cluster values live in this std430 storage buffer, one record per draw,
// indexed by gl_DrawID in the vertex shader.
struct VirtualGeometryDrawRecord {
    std::uint32_t id_lo = 0;        // packed v3 visibility ID, low word
    std::uint32_t id_hi = 0;        // packed v3 visibility ID, high word
    std::uint32_t index_base = 0;   // this cluster's first index in the global index buffer
    std::uint32_t vertex_base = 0;  // this cluster's first vertex in the global vertex buffer
    std::uint32_t stride_words = 0; // cooked vertex stride in u32 words
    std::uint32_t cluster_slot = 0; // cluster slot this draw owns
    std::uint32_t pad[2] = {};      // pad to 32 bytes for std430
};

static_assert(sizeof(VirtualGeometryDrawRecord) == 32, "must match the shaders' std430 struct");

inline constexpr std::uint32_t kVirtualGeometryMaxIndirectDraws = 1024u;

struct VirtualGeometryClusterBuffers {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::BufferHandle clusters;
    rhi::BufferHandle records;             // per-draw VirtualGeometryDrawRecord storage buffer
    rhi::BufferHandle indirect;            // constant-capacity VkDrawIndexedIndirectCommand array
    rhi::BufferHandle identity_index;      // identity sequence 0,1,2,... for vertex pulling
    std::uint32_t cluster_count = 0;       // entries in `clusters` (max drawn slot + 1)
    std::uint32_t vertex_stride_words = 0; // cooked vertex stride / 4
};

// Every way a request can draw nothing gets its own counter (the replication rule applied to
// rendering: a proof that cannot see what it skipped still reads as passing).
struct VirtualGeometryVisibilityStats {
    std::uint32_t drawn = 0;
    std::uint32_t skipped_invalid_request = 0; // null pointers / invalid asset / bad cluster index
    std::uint32_t skipped_not_selected = 0;    // cluster's group is not in the selected cut
    std::uint32_t skipped_not_leaf = 0;        // selected group still has children
    std::uint32_t skipped_not_resident = 0;    // cluster page (or a dependency) not resident
    std::uint32_t skipped_page_view = 0;       // view_virtual_geometry_page() rejected the page
    std::uint32_t skipped_bad_index = 0;       // an index points outside the cluster's vertices
    std::uint32_t skipped_bad_id = 0;          // slot/generation do not fit the visibility ABI
    std::uint32_t skipped_too_many_triangles = 0; // > 128 triangles: the ID has 7 triangle bits
    std::uint32_t skipped_duplicate_slot = 0;     // a slot already used earlier in this request
    std::uint32_t skipped_over_capacity = 0; // accepted cluster past the fixed indirect draw count
    // The device cannot do GPU-driven draw (AdapterInfo::gpu_driven_draw). Every cluster in the
    // request lands here: this path has no non-indirect fallback, and drawing a truncated cut
    // would look like a selection bug a long way from its cause.
    std::uint32_t skipped_no_gpu_driven_draw = 0;
};

class VirtualGeometryVisibilityPass {
public:
    explicit VirtualGeometryVisibilityPass(rhi::Device& device);
    ~VirtualGeometryVisibilityPass();

    VirtualGeometryVisibilityPass(const VirtualGeometryVisibilityPass&) = delete;
    VirtualGeometryVisibilityPass& operator=(const VirtualGeometryVisibilityPass&) = delete;

    // Declare one raster pass into `graph` that clears `visibility` (RG32Uint) and `depth_bits`
    // (R32Uint, floatBitsToUint of window-space depth) to zero and draws every accepted cluster.
    // A rejected cluster bumps exactly one skip counter; if all are rejected the clearing pass is
    // still declared — the target must be "nothing" rather than stale. Returns whether anything
    // was drawn.
    //
    // The uploaded cluster buffers are replaced by the next declare(); the caller must have
    // waited for the submission that used the previous ones (one visibility frame in flight).
    bool declare(RenderGraph& graph,
                 RGTexture visibility,
                 RGTexture depth_bits,
                 RGTexture depth,
                 const VirtualGeometryVisibilityRequest& request);

    [[nodiscard]] const VirtualGeometryVisibilityStats& stats() const noexcept { return stats_; }

    // What the last declare() uploaded — the resolve pass's input. See
    // VirtualGeometryClusterBuffers.
    [[nodiscard]] const VirtualGeometryClusterBuffers& cluster_buffers() const noexcept {
        return buffers_;
    }

private:
    void release_cluster_buffers() noexcept;

    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    VirtualGeometryClusterBuffers buffers_;
    VirtualGeometryVisibilityStats stats_;
};

} // namespace rime::render
