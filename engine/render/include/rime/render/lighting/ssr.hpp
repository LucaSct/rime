// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>

#include "rime/core/math/mat.hpp"
#include "rime/render/render_graph.hpp"

// Screen-space reflections — the resolve pass (m10.7b, ADR-0032 §5).
//
// SSR gives the specular term a real environment to reflect, taken from the frame the renderer just
// drew. A fullscreen pass, one fragment per pixel: reconstruct the surface from the depth buffer +
// the m10.7a G-buffer, reflect the view ray about the surface normal, and MARCH that ray through
// the depth buffer in view space until it crosses behind a depth sample (a hit) or leaves the
// screen (a miss, which falls back to the flat sky/ambient term). The reflection, weighted by
// Fresnel and a roughness fade, is added to the pixel's lit colour and written to a SECOND HDR
// target the tonemap then reads — so with SSR off the tonemap reads the original HDR unchanged and
// the frame is byte-identical (ADR-0032 §11).
//
// It is a fullscreen raster pass (not a compute dispatch): v1's fixed linear march needs nothing
// compute offers — no groupshared, no scatter — so a fragment shader writing a colour attachment is
// the simpler realization, and the tonemap samples that attachment through the same path every
// forward→tonemap frame already uses. Compute earns its keep at m10.7c (a hi-Z march wants a shared
// depth pyramid; temporal accumulation wants history) behind this same seam. See ssr_resolve.frag's
// header and docs/math/ssr.md.
//
// m10.7c turns the two named follow-ups on: a MISS (a ray off the screen, into the background, or
// past the roughness knob) now falls back to the DDGI probe field sampled in the reflection
// direction — reflections everywhere, coupled to the GI thesis — instead of a flat sky constant;
// and the roughness fade becomes a CONE, blending the sharp screen hit toward that (inherently
// low-frequency, pre-integrated) probe field as roughness rises, so a rough surface shows a BLURRED
// reflection rather than nothing. Still deferred, with named triggers: a hi-Z acceleration march
// (m12.0 — pure performance, unmeasurable on lavapipe's exact CPU depth, and the linear march is
// already correct) and a mip-chained SCREEN-colour blur for the mid-roughness band (needs an RHI
// mip-generation/LOD surface that does not exist yet; the probe cone covers rough reflections
// without it). See ssr_resolve.frag and docs/math/ssr.md §5/§6.
//
// With DDGI off the probe fallback reduces to the flat ambient constant it replaced, so the whole
// pass is byte-for-byte the m10.7b behaviour (and, SSR off, the pre-M10 baseline).
// `ssr_resolve.frag` mirrors the GpuSsrUniforms block below.
namespace rime::render {

// What the resolve needs to know about the view it marches. `view` (world → view) rotates the
// G-buffer's world normal into the view space the march happens in; the projection is passed as the
// matrix itself (SSR needs its inverse too, computed once on the CPU here) rather than re-derived,
// because the march reconstructs positions, not just froxel bounds.
struct SsrInputs {
    core::Mat4 view; // world → view
    core::Mat4 proj; // clip-from-view (core::perspective)
    float z_near = 0.1f;
    float z_far = 1000.0f;
    rhi::Extent2D extent{};
    float ambient[3] = {0.0f, 0.0f, 0.0f}; // the sky a missed ray reflects — matches the forward's
    float max_distance = 8.0f;             // how far (view units) a ray marches before it is a miss
    float thickness = 0.5f;       // view-space depth tolerance for "the ray hit this pixel"
    std::uint32_t max_steps = 48; // fixed linear steps along the ray
};

// std140 twin of the SsrParams block in ssr_resolve.frag. Every member is a mat4 or a 16-byte vec4,
// so C++'s layout and std140 agree by construction (the GpuFrameUniforms / GpuClusterUniforms
// discipline). `inv_view` (m10.7c) is what the probe fallback needs: it turns the march's
// view-space surface point and reflection ray back into WORLD space, where the DDGI lattice lives.
struct GpuSsrUniforms {
    core::Mat4 proj;               // 0
    core::Mat4 inv_proj;           // 64
    core::Mat4 view;               // 128
    core::Mat4 inv_view;           // 192: view → world, for the m10.7c probe fallback
    float extent_near_far[4] = {}; // 256: xy = extent px, z = near, w = far
    float params[4] = {};  // 272: x = max_steps, y = thickness, z = unused, w = max_distance
    float ambient[4] = {}; // 288: rgb = miss fallback (the flat sky, when DDGI is off)
};

static_assert(sizeof(GpuSsrUniforms) == 304 && offsetof(GpuSsrUniforms, inv_proj) == 64 &&
                  offsetof(GpuSsrUniforms, view) == 128 &&
                  offsetof(GpuSsrUniforms, inv_view) == 192 &&
                  offsetof(GpuSsrUniforms, extent_near_far) == 256 &&
                  offsetof(GpuSsrUniforms, params) == 272 &&
                  offsetof(GpuSsrUniforms, ambient) == 288,
              "GpuSsrUniforms no longer matches the std140 SsrParams block");

// Owns the SSR resolve pipeline (a fullscreen raster pass), its uniform block, and the sampler the
// march reads depth/colour through. One per SceneRenderer; only touched when ssr_enabled.
class SsrPass {
public:
    explicit SsrPass(rhi::Device& device);
    ~SsrPass();

    SsrPass(const SsrPass&) = delete;
    SsrPass& operator=(const SsrPass&) = delete;

    // Declare the resolve pass: sample `scene_color` (the lit HDR), `gbuffer`, and `depth`; write
    // `out_hdr` = scene_color + reflection as a colour attachment. The caller creates `out_hdr` (a
    // second HDR target) and, when SSR is on, feeds THAT to the tonemap instead of the raw
    // scene_color.
    //
    // The last four arguments are the m10.7c probe fallback: the DDGI irradiance + visibility
    // atlases, the GpuDdgiSampleParams uniform, and the atlases' shared linear+clamp sampler — i.e.
    // exactly a DdgiBinding's fields (taken raw so this leaf pass need not include passes.hpp's
    // mesh/material graph). A ray that leaves the screen, hits the background, or belongs to a
    // rough surface samples the irradiance field in the REFLECTION direction instead of the flat
    // sky. Pass DdgiProbes::empty_binding's members when DDGI is off: its sample-params carry
    // `enabled = 0`, so the shader takes the flat-ambient path and the frame is byte-identical to
    // m10.7b.
    void add(RenderGraph& graph,
             RGTexture scene_color,
             RGTexture gbuffer,
             RGTexture depth,
             RGTexture out_hdr,
             const SsrInputs& inputs,
             RGTexture ddgi_irradiance,
             RGTexture ddgi_visibility,
             rhi::BufferHandle ddgi_params,
             rhi::SamplerHandle ddgi_sampler);

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;   // fullscreen.vert — the oversized-triangle idiom
    rhi::ShaderHandle fragment_shader_; // ssr_resolve.frag
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle uniforms_;
    rhi::SamplerHandle sampler_; // point + clamp: depth must not interpolate, edges must not wrap
};

} // namespace rime::render
