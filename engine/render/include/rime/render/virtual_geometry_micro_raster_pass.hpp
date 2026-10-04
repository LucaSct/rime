// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>

#include "rime/render/micro_triangle.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"

// M18.4: the software half of the HYBRID micro-triangle rasterizer (ADR-0043 gate 6, ADR-0058).
//
// ADR-0043 makes the split mandatory: triangles that cover enough pixels go through the hardware
// visibility pass (virtual_geometry_visibility_pass.hpp), sub-pixel ones through a compute
// rasterizer, and the two must agree on depth, identity and silhouette winners at their boundary.
// This pass is the compute half. It runs AFTER the hardware pass and writes into the same three
// targets — the RG32Uint visibility ID, the R32Uint depth bits and the D32 depth — so every
// consumer downstream (the material resolve, picking) sees one visibility buffer and cannot tell
// which rasterizer produced a pixel.
//
// Per declare(), three or four graph passes:
//   vg-micro-clear       compute: the per-pixel sample buffer := all ones ("empty");
//   vg-micro-raster      compute: one thread per triangle, 64-bit atomicMin of (depth key, index);
//     (or, without 64-bit atomics, vg-micro-raster-depth + vg-micro-raster-id: two 32-bit passes
//      that compute the same lexicographic minimum — see vg_micro_raster.comp);
//   vg-micro-merge       raster: a fullscreen resolve whose DEPTH TEST arbitrates hardware versus
//                        software, loading the hardware pass's attachments.
//
// What goes in is already projected and already routed: route_micro_triangle() (micro_triangle.hpp)
// is the classifier wiring, and a caller hands this pass the Software-routed list. Moving the
// projection and the per-triangle routing onto the GPU — so a cluster's triangles split without a
// CPU round trip — is the next brick; see ADR-0058's "not yet" list.
//
// Stub/limits, stated: the triangle list is uploaded per declare() into a host-visible buffer (as
// the hardware pass does with its clusters, until the GPU page pool exists); one declare() per
// frame in flight, and the previous frame's submission must have completed before the next one.
namespace rime::render {

// One software-raster input: the projected triangle plus the visibility ID its samples carry
// (triangle field already filled — the software path names triangles exactly as the hardware
// path's vg_visibility.frag does). Mirrors the MicroTriangle struct in vg_micro_raster.comp.
struct VirtualGeometryMicroTriangle {
    ProjectedTriangle triangle;
    VirtualGeometryVisibilityWords id;
    std::uint32_t pad = 0;
};

static_assert(sizeof(ProjectedTriangle) == 36, "nine floats, as the shader reads them");
static_assert(sizeof(VirtualGeometryMicroTriangle) == 48, "must match the shaders' std430 struct");

// Per-declare upload ceiling. The raster dispatch is one thread per triangle in groups of 64; this
// keeps the group count far below the 65,535 dispatch limit.
inline constexpr std::uint32_t kVirtualGeometryMaxMicroTriangles = 1u << 20;

// What the GPU saw. Mirrors the Counters block in vg_micro_raster.comp; read back by tests and
// tooling only — no frame decision waits on it. Every way a triangle can produce no samples has a
// counter of its own.
struct VirtualGeometryMicroRasterCounters {
    std::uint32_t processed = 0;
    std::uint32_t rejected_invalid_id = 0;
    std::uint32_t rejected_invalid = 0;
    std::uint32_t rejected_degenerate = 0;
    std::uint32_t rejected_inverse = 0;
    std::uint32_t dropped_oversized = 0;
    std::uint32_t culled_backface = 0;
    std::uint32_t no_sample = 0;
    std::uint32_t rasterized = 0;
    std::uint32_t samples_covered = 0;
    std::uint32_t non_finite_depth = 0;
};

static_assert(sizeof(VirtualGeometryMicroRasterCounters) == 44,
              "must match the Counters block in vg_micro_raster.comp");

// How the per-pixel minimum is computed. Auto uses 64-bit atomics when the adapter has them
// (AdapterInfo::buffer_int64_atomics); Portable32 forces the two-pass 32-bit path everywhere, which
// is how the tests keep the fallback exercised on a device that would never pick it.
enum class MicroRasterAtomics : std::uint8_t { Auto, Portable32 };

struct VirtualGeometryMicroRasterRequest {
    // Software-routed triangles in framebuffer pixels (see ProjectedTriangle). List ORDER is part
    // of the contract: between equal depths the earlier entry wins, as in the CPU oracle.
    std::span<const VirtualGeometryMicroTriangle> triangles = {};
    // The target size in pixels; must be the extent of all three targets.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Mirror the hardware pass's back-face cull (CullMode::Back, counter-clockwise front). Off
    // makes the pass the CPU oracle's exact twin, which rasterizes both windings.
    bool cull_back_faces = true;
    // Triangles whose bounding box is wider or taller than this are counted and dropped; the router
    // keeps them out in the first place (kMicroTriangleMaxSoftwareExtentPx).
    float max_extent_px = kMicroTriangleMaxSoftwareExtentPx;
    MicroRasterAtomics atomics = MicroRasterAtomics::Auto;
};

// CPU-side outcome of the last declare(). As with the hardware pass, a count of what was UPLOADED,
// never a claim about what was drawn — that is VirtualGeometryMicroRasterCounters.
struct VirtualGeometryMicroRasterStats {
    std::uint32_t offered = 0;               // entries in request.triangles
    std::uint32_t uploaded = 0;              // entries sent to the GPU
    std::uint32_t skipped_over_capacity = 0; // past kVirtualGeometryMaxMicroTriangles
    std::uint32_t skipped_bad_request = 0;   // zero/mismatched size: the whole request
    std::uint32_t used_int64_atomics = 0;    // 1 = the single-pass 64-bit path ran
};

class VirtualGeometryMicroRasterPass {
public:
    explicit VirtualGeometryMicroRasterPass(rhi::Device& device);
    ~VirtualGeometryMicroRasterPass();

    VirtualGeometryMicroRasterPass(const VirtualGeometryMicroRasterPass&) = delete;
    VirtualGeometryMicroRasterPass& operator=(const VirtualGeometryMicroRasterPass&) = delete;

    // Declare the software raster into `graph`, AFTER the hardware visibility pass has written
    // `visibility`, `depth_bits` and `depth` (it must store its depth). Declares nothing when there
    // is nothing to rasterize, so the hardware result stands untouched. Returns whether passes were
    // declared.
    bool declare(RenderGraph& graph,
                 RGTexture visibility,
                 RGTexture depth_bits,
                 RGTexture depth,
                 const VirtualGeometryMicroRasterRequest& request);

    [[nodiscard]] const VirtualGeometryMicroRasterStats& stats() const noexcept { return stats_; }

    // Host-readable VirtualGeometryMicroRasterCounters of the last declare() (exported from the
    // graph, so valid after execute()). Invalid when nothing was declared.
    [[nodiscard]] rhi::BufferHandle counters() const noexcept { return counters_; }

private:
    void release_buffers() noexcept;

    rhi::Device& device_;
    rhi::ShaderHandle clear_shader_;
    rhi::ShaderHandle raster64_shader_;
    rhi::ShaderHandle depth32_shader_;
    rhi::ShaderHandle id32_shader_;
    rhi::ShaderHandle merge_vs_;
    rhi::ShaderHandle merge_fs_;
    rhi::PipelineHandle clear_pipeline_;
    rhi::PipelineHandle raster64_pipeline_;
    rhi::PipelineHandle depth32_pipeline_;
    rhi::PipelineHandle id32_pipeline_;
    rhi::PipelineHandle merge_pipeline_;
    rhi::BufferHandle triangles_;
    rhi::BufferHandle counters_;
    VirtualGeometryMicroRasterStats stats_;
};

} // namespace rime::render
