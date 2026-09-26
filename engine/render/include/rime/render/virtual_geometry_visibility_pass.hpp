// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/virtual_geometry.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_gpu_selection.hpp"
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
    // GPU-BUILT SUBMISSION (M18.3c), opt-in. When this holds the flag buffer that
    // select_virtual_geometry_on_gpu left on the device, the "is this cluster on the cut?" gate
    // moves off the CPU: every cluster that passes the static and residency gates is uploaded as a
    // CANDIDATE, and a compute dispatch turns the GPU's verdict into the indirect commands. That is
    // what ADR-0043 gate 4 asks for — no readback decides this frame's draw list.
    //
    // `selection` is still required, and must still be the cut: it is what the CPU path gates on,
    // and on the GPU path it is what the test compares the GPU's choice against. The two must
    // describe the same frame or the comparison is meaningless.
    // The buffer must outlive the frame: declare() imports it into the graph, so it is read when
    // the graph executes, not when declare() returns.
    const VirtualGeometryGpuSelectionBuffers* gpu_selection = nullptr;
    // Effective draw capacity, clamped to kVirtualGeometryMaxIndirectDraws; 0 means the maximum. On
    // the GPU path it is the builder's capacity (and the CPU keeps uploading candidates up to
    // kVirtualGeometryMaxDrawCandidates); on the CPU path it caps what declare() accepts, counting
    // the rest into skipped_over_capacity. It does NOT change the submitted draw count, which stays
    // the constant kVirtualGeometryMaxIndirectDraws — commands past the capacity are zeroed no-ops.
    // It exists so the overflow-to-coarse path is reachable with a handful of clusters instead of
    // 1025 of them, and so a future frame budget has somewhere to land.
    std::uint32_t max_draws = 0;
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

// One cluster the GPU builder MAY turn into a draw. The CPU owns the gates only it can answer —
// static asset validity, residency, the visibility-ID bounds, slot uniqueness — and uploads
// everything that survives them; the builder owns exactly one gate, "is this cluster's group on the
// cut?", because that is the answer that lives in GPU memory.
struct VirtualGeometryDrawCandidate {
    VirtualGeometryDrawRecord record; // emitted verbatim when the candidate becomes a draw
    std::uint32_t index_count = 0;    // indices this cluster draws
    std::uint32_t group = 0;          // replacement group whose selected flag gates it
    // 1 = this cluster belongs to the permanently-resident coarse group, so it is a legal fallback
    // when the full cut does not fit the command buffer.
    std::uint32_t coarse = 0;
    std::uint32_t pad = 0;
};

static_assert(sizeof(VirtualGeometryDrawCandidate) == 48,
              "must match the Candidate struct in vg_build_draws.comp");

// Upload ceiling for candidates. The builder's compaction scan is O(N^2) flag reads, which is free
// at this N against a buffer this small; a larger ceiling wants a prefix sum first.
inline constexpr std::uint32_t kVirtualGeometryMaxDrawCandidates = 4096u;

// What the GPU builder decided, as IT saw it. Read back by tests and tooling only — the frame path
// never waits on this, which is the whole point. Mirrors the BuildCounters block in
// vg_build_draws.comp.
struct VirtualGeometryGpuBuildCounters {
    std::uint32_t selected_total = 0;       // candidates on the cut, before capacity
    std::uint32_t emitted = 0;              // draw commands written
    std::uint32_t fell_back_to_coarse = 0;  // the cut overflowed; the coarse cut was drawn
    std::uint32_t coarse_over_capacity = 0; // even the coarse cut was truncated
    // The cut overflowed and no coarse candidate was offered, so NOTHING was drawn. The candidate
    // upload has its own ceiling (kVirtualGeometryMaxDrawCandidates) and the coarse cluster can be
    // the one it drops; this is the counter that keeps that from being an invisible hole.
    std::uint32_t overflow_without_coarse = 0;
};

struct VirtualGeometryClusterBuffers {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::BufferHandle clusters;
    rhi::BufferHandle records;             // per-draw VirtualGeometryDrawRecord storage buffer
    rhi::BufferHandle indirect;            // constant-capacity VkDrawIndexedIndirectCommand array
    rhi::BufferHandle identity_index;      // identity sequence 0,1,2,... for vertex pulling
    rhi::BufferHandle candidates;          // GPU path only: VirtualGeometryDrawCandidate array
    rhi::BufferHandle build_counters;      // GPU path only: VirtualGeometryGpuBuildCounters
    std::uint32_t cluster_count = 0;       // entries in `clusters` (max drawn slot + 1)
    std::uint32_t vertex_stride_words = 0; // cooked vertex stride / 4
    std::uint32_t candidate_count = 0;     // entries in `candidates` (0 on the CPU path)
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
    std::uint32_t skipped_over_candidate_cap =
        0; // GPU path: past kVirtualGeometryMaxDrawCandidates
    // request.gpu_selection was given but carries no verdict buffer. As with
    // skipped_no_gpu_driven_draw the WHOLE request lands here: the safe reading of a missing
    // verdict is "nothing is selected", and quietly gating on the CPU's copy instead would make a
    // broken GPU path look like a working one.
    std::uint32_t skipped_no_gpu_verdict = 0;
    // GPU path: candidates uploaded and offered to the builder. `drawn` stays 0 on this path, on
    // purpose — the CPU does not know what was drawn, and a counter that reported an intention as a
    // fact is the exact failure the counter rule exists to prevent. What the GPU chose is in
    // VirtualGeometryGpuBuildCounters.
    std::uint32_t candidates_offered = 0;
    std::uint32_t gpu_built = 0; // 1 = the last declare() built its commands on the GPU
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
    //
    // With request.gpu_selection set, a "vg-build-draws" COMPUTE pass is declared first and the
    // raster pass reads what it wrote. Two gates then behave differently, and both are deliberate:
    // `skipped_not_selected` can no longer fire, because that verdict is the GPU's; and a cluster
    // of the COARSE group is accepted even though the group has children, because the coarse cut is
    // the overflow fallback and must be on the device before the overflow is known. The return
    // value then means "commands were built", not "pixels were drawn" — what the GPU chose is in
    // VirtualGeometryGpuBuildCounters, read from cluster_buffers().build_counters.
    //
    // Known cost of that seam, stated rather than hidden: the CPU uploads the geometry of every
    // candidate, including the coarse clusters it will usually not draw, because it no longer knows
    // which ones win. That is the price of not reading the verdict back, and it goes away with the
    // GPU page pool (M18 step 5), not before.
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
    rhi::ShaderHandle build_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::PipelineHandle build_pipeline_;
    VirtualGeometryClusterBuffers buffers_;
    VirtualGeometryVisibilityStats stats_;
};

} // namespace rime::render
