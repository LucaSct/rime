// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/render/passes.hpp"

namespace rime::render {

// A second camera supplies HDR radiance; this pass puts it on the physical ocular surface.
// Schedule after first-person depth/colour and before parent TAA. It never changes parent FOV.
class ScopeLensPass {
public:
    struct Params {
        core::Mat4 main_view_proj; // same jittered VP used for the lens depth draw
        core::Vec3 main_eye{};
        core::Mat4 optic_from_world; // rigid optic frame: ocular at origin, eye on +Z, aim along -Z
        core::Mat4 optical_view_proj; // unjittered second VP when scope HDR has been TAA-resolved
        float lens_radius = 0.025f;   // radius in optic-space metres (independent of mesh scale)
        float eye_relief = 0.09f;
        float eye_relief_tolerance = 0.06f;
        float eye_box_radius = 0.012f;
        float focus_distance = 100.0f;
        float optical_tan_half_fov = 0.04f; // square scope render; vertical half-FOV tangent
        float intensity = 1.0f;
        float reflection_strength = 1.0f; // zero is a useful sensitivity control
        float glass_roughness = 0.12f;
        float reticle_strength = 1.0f;
    };

    explicit ScopeLensPass(rhi::Device& device);
    ~ScopeLensPass();
    ScopeLensPass(const ScopeLensPass&) = delete;
    ScopeLensPass& operator=(const ScopeLensPass&) = delete;

    // The mesh must be the SAME undeformed indexed lens drawn into model_depth. Its local
    // normals face the eye; optic_from_world defines its physical XY aperture. All graph handles
    // belong to this graph. Invalid parameters/absent scope return parent_hdr without a pass.
    // A valid prefiltered_sky is the engine's sampler2DArray GGX chain, not a cubemap.
    [[nodiscard]] RGTexture add(RenderGraph& graph,
                                RGTexture parent_hdr,
                                RGTexture model_depth,
                                RGTexture scope_hdr,
                                RGTexture prefiltered_sky,
                                rhi::Extent2D extent,
                                const MeshRegistry& meshes,
                                const DrawItem& lens,
                                const Params& params) const;

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_, fragment_shader_, sky_fragment_shader_;
    rhi::PipelineHandle pipeline_, sky_pipeline_;
    rhi::SamplerHandle sampler_;
};

} // namespace rime::render
