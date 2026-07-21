// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"

// Directional cascaded shadow maps (m10.1, ADR-0032 §3). The sun casts real shadows: the scene
// depth is rendered from the light into N cascades (progressively coarser slices of the view
// frustum), stored in one array texture (the m10.1a array-texture RHI), and sampled in the forward
// pass with a depth-compare (sampler2DArrayShadow) for hardware PCF. The cascade depth render
// REUSES the depth pre-pass verbatim (depth_only.vert + DepthPrepass's pipeline) aimed from the
// light instead of the camera — a cascade is structurally the same pass with a different view_proj
// and a target layer.
//
// The whole "why" — the shadow test as a re-projection, practical-split cascades, texel snapping,
// slope/normal bias, PCF — is derived in docs/math/shadow-mapping.md. This header cites; the doc
// explains.
namespace rime::render {

// The GPU mirror (std140) of the ShadowUniforms block in pbr_forward_shadowed.frag. Every member is
// a mat4 or a 16-byte vec4, so C++'s natural layout and std140 agree (the vec3 padding trap is
// dodged by construction, exactly like GpuFrameUniforms). The static_assert below is the tripwire.
struct GpuShadowUniforms {
    core::Mat4 cascade_view_proj[kMaxCascades]; // light clip-from-world, one per cascade
    // x = cascade count (as float), y = PCF radius in texels, z = depth bias, w = normal-offset
    // bias.
    float params[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float texel[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // x = 1 / shadow_map_resolution; yzw unused
};

static_assert(sizeof(GpuShadowUniforms) == kMaxCascades * 64 + 32 &&
                  offsetof(GpuShadowUniforms, params) == kMaxCascades * 64,
              "GpuShadowUniforms no longer matches the std140 ShadowUniforms block in the shader");

// The camera + sun state the cascade fit needs, pulled from the extracted scene (GPU-free, so the
// math is unit-testable without a device). `light_dir` is the direction the sun TRAVELS (world),
// the same convention the forward pass uses.
struct CascadeInputs {
    core::Mat4 camera_view; // world → view (ExtractedCamera::view)
    float fov_y = 0.87266f;
    float aspect = 1.0f;
    float z_near = 0.1f;
    float z_far = 100.0f;
    core::Vec3 light_dir = {0.0f, -1.0f, 0.0f};
};

// The fit result: a light view_proj per cascade, and how many are valid. Pure output of
// compute_cascades — no GPU, so a test pins the split distances and the texel-snap stability.
struct CascadeFit {
    core::Mat4 view_proj[kMaxCascades];
    std::uint32_t count = 0;
};

// Fit `settings.cascade_count` cascades to the camera frustum for a sun travelling `light_dir`.
// Each cascade bounds a depth slice of the frustum with a rotation-invariant sphere (stable radius
// ⇒ stable texel size ⇒ no shimmer under rotation) and snaps its centre to whole shadow-map texels
// (no shimmer under translation). Derivation: docs/math/shadow-mapping.md §2–3.
[[nodiscard]] CascadeFit compute_cascades(const CascadeInputs& in,
                                          const LightingSettings& settings);

// Owns the shadow GPU resources (the depth-compare sampler, the per-cascade view_proj buffer the
// depth passes read, and the ShadowUniforms buffer the forward pass reads) and declares the cascade
// depth passes each frame. One per SceneRenderer.
class CascadedShadowMap {
public:
    explicit CascadedShadowMap(rhi::Device& device);
    ~CascadedShadowMap();

    CascadedShadowMap(const CascadedShadowMap&) = delete;
    CascadedShadowMap& operator=(const CascadedShadowMap&) = delete;

    // Declare the frame's cascade depth passes into `graph` (reusing `prepass` aimed from the
    // light), upload the cascade matrices, and return what the shadowed forward pass binds.
    // `scene_data` is the same draw list the forward pass uses — the cascade passes render its
    // geometry's depth.
    [[nodiscard]] ShadowBinding add(RenderGraph& graph,
                                    const DepthPrepass& prepass,
                                    const SceneDrawData& scene_data,
                                    const CascadeInputs& inputs,
                                    const LightingSettings& settings);

    // The valid-but-empty cascade binding for a shadowed frame with NO directional light (a
    // spot-only scene, m10.2): the shadowed pipeline's binding 7/8 must still point at a real
    // 2-D-array depth image + a count-0 uniform block. `dummy` is the SceneRenderer's shared
    // placeholder array.
    [[nodiscard]] ShadowBinding empty_binding(RenderGraph& graph, rhi::TextureHandle dummy);

private:
    rhi::Device& device_;
    rhi::SamplerHandle compare_sampler_; // sampler2DArrayShadow: LessEqual hardware PCF
    rhi::BufferHandle cascade_vp_ubo_;   // kMaxCascades × 256-byte slices, each a light view_proj
    rhi::BufferHandle shadow_ubo_;       // GpuShadowUniforms for the forward pass
    std::uint32_t resolution_ = 0;       // the array texture's current per-cascade size
};

} // namespace rime::render
