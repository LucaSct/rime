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
// shading while rasterizing, write only *which cluster* covers each pixel into an integer target,
// and let a later pass fetch attributes and shade exactly once per pixel. Here the integer is the
// packed R32Uint visibility-ID ABI (virtual_geometry_visibility_id.hpp) — cluster slot, generation
// and format version — so a recycled slot cannot be confused with an old pixel.
//
// This brick deliberately reuses the M9.6 picker's machinery rather than inventing a new path:
// the same pick_id.vert (MVP + one uint in push constants), the same "clear an integer target to
// all-zero bits through the float-shaped clear" trick (0 is also the ABI's invalid ID), and the
// same Less depth test so the nearest cluster wins. The only new shader is a fragment stage that
// ALSO writes gl_FragCoord.z's bits to a second R32Uint target: the RHI's depth readback copies
// the colour aspect only, so encoding depth into a copyable integer is how a test (and a future
// debug view) can inspect it without widening the RHI.
//
// Stub/limits (M18 step 1): exactly ONE cluster per call, uploaded from its page bytes through
// view_virtual_geometry_page() into host-visible buffers owned by this pass. There is no GPU page
// pool, no instancing, and no cluster culling yet — those are later M18 steps.
namespace rime::render {

struct VirtualGeometryVisibilityRequest {
    const assets::VirtualGeometryAsset* asset = nullptr;
    assets::AssetId asset_id{};
    const VirtualGeometryResidency* residency = nullptr;
    // The cut produced by select_virtual_geometry(); the cluster must belong to one of these
    // groups, and that group must be a leaf (no children) — this pass draws leaf clusters only.
    const VirtualGeometrySelection* selection = nullptr;
    std::uint32_t cluster = assets::kInvalidVirtualGeometryIndex;
    // The residency slot and allocation generation that the upload scheduler would assign. Until
    // that scheduler exists the caller supplies them; they are packed into the pixel verbatim.
    std::uint32_t cluster_slot = 0;
    std::uint32_t generation = 0;
    core::Mat4 clip_from_object{};
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
};

class VirtualGeometryVisibilityPass {
public:
    explicit VirtualGeometryVisibilityPass(rhi::Device& device);
    ~VirtualGeometryVisibilityPass();

    VirtualGeometryVisibilityPass(const VirtualGeometryVisibilityPass&) = delete;
    VirtualGeometryVisibilityPass& operator=(const VirtualGeometryVisibilityPass&) = delete;

    // Declare one raster pass into `graph` that clears `visibility` (R32Uint) and `depth_bits`
    // (R32Uint, floatBitsToUint of window-space depth) to zero and, if the request is valid,
    // draws the cluster. A rejected request still declares the clearing pass — the target must be
    // "nothing" rather than stale — and bumps exactly one skip counter. Returns whether it drew.
    //
    // The uploaded cluster buffers are replaced by the next declare(); the caller must have
    // waited for the submission that used the previous ones (one visibility frame in flight).
    bool declare(RenderGraph& graph,
                 RGTexture visibility,
                 RGTexture depth_bits,
                 RGTexture depth,
                 const VirtualGeometryVisibilityRequest& request);

    [[nodiscard]] const VirtualGeometryVisibilityStats& stats() const noexcept { return stats_; }

private:
    void release_cluster_buffers() noexcept;

    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle vertices_;
    rhi::BufferHandle indices_;
    VirtualGeometryVisibilityStats stats_;
};

} // namespace rime::render
