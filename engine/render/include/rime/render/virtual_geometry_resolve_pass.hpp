// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>

#include "rime/core/math/mat.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_visibility_pass.hpp"

// M18 step 2: the **material resolve** half of the visibility buffer.
//
// Step 1 left one packed integer per pixel: which triangle of which resident cluster won the
// depth test. This pass turns that back into what a forward pass would have known at the same
// pixel — the material, the interpolated UV, the UV's screen-space gradients, and the albedo
// sampled with them — by re-fetching the triangle's three vertices from the cluster storage
// buffers and reconstructing the pixel's barycentrics analytically (the technique is written up
// in shaders/vg_resolve.frag). Shading therefore runs exactly once per covered pixel, whatever
// the overdraw was, and at a cost independent of how many triangles the cluster has.
//
// Outputs, all written by one fullscreen triangle (fullscreen.vert):
//   material_ids (R32Uint)     — material_slot + 1; 0 where the visibility ID is empty;
//                                kVirtualGeometryResolveStale where a pixel carries an ID the
//                                cluster table cannot resolve (wrong version or generation,
//                                unknown slot, triangle past the cluster's count). A covered
//                                pixel is never silently resolved to "empty".
//   uv (RGBA32Float)           — (u, v, du/dx, dv/dy); du/dy and dv/dx are used for sampling but
//                                not stored.
//   albedo (RGBA8Unorm)        — textureGrad of the slot's albedo texture.
//
// Stub/limits (M18 step 2): reads the cluster buffers the visibility pass uploaded (no GPU page
// pool yet), one object transform for all clusters, at most kVirtualGeometryResolveMaxMaterials
// albedo textures and no other material inputs. Deferred: normal/tangent reconstruction and
// normal maps, lighting, and picking parity through the visibility target.
namespace rime::render {

inline constexpr std::uint32_t kVirtualGeometryResolveMaxMaterials = 4;
inline constexpr std::uint32_t kVirtualGeometryResolveStale = 0xffffffffu;

// One material slot's inputs. The sampler decides filtering, so trilinear sampling makes the
// resolve's gradients (and therefore its mip choice) observable in the albedo.
struct VirtualGeometryResolveMaterial {
    rhi::TextureHandle albedo;
    rhi::SamplerHandle sampler;
};

struct VirtualGeometryResolveRequest {
    // What the visibility pass uploaded for the frame whose target is being resolved.
    const VirtualGeometryClusterBuffers* clusters = nullptr;
    // The matrix the visibility pass drew with; the barycentrics are only exact if it matches.
    core::Mat4 clip_from_object{};
    // Indexed by VirtualGeometryCluster::material_slot; 1..kVirtualGeometryResolveMaxMaterials.
    std::span<const VirtualGeometryResolveMaterial> materials = {};
};

struct VirtualGeometryResolveStats {
    std::uint32_t resolved = 0;
    std::uint32_t skipped_no_clusters = 0;   // nothing was drawn: outputs are only cleared
    std::uint32_t skipped_bad_materials = 0; // no materials, too many, or an invalid handle
};

class VirtualGeometryResolvePass {
public:
    explicit VirtualGeometryResolvePass(rhi::Device& device);
    ~VirtualGeometryResolvePass();

    VirtualGeometryResolvePass(const VirtualGeometryResolvePass&) = delete;
    VirtualGeometryResolvePass& operator=(const VirtualGeometryResolvePass&) = delete;

    // Declare one fullscreen raster pass that clears the three outputs to zero and, if the
    // request is usable, resolves every pixel of `visibility` (R32Uint, sampled). A rejected
    // request bumps exactly one skip counter and leaves the outputs cleared. Returns whether it
    // resolved. `extent` is the visibility target's size in pixels.
    bool declare(RenderGraph& graph,
                 RGTexture visibility,
                 rhi::Extent2D extent,
                 RGTexture material_ids,
                 RGTexture uv,
                 RGTexture albedo,
                 const VirtualGeometryResolveRequest& request);

    [[nodiscard]] const VirtualGeometryResolveStats& stats() const noexcept { return stats_; }

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle id_sampler_;
    VirtualGeometryResolveStats stats_;
};

} // namespace rime::render
