// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/ecs/world.hpp"
#include "rime/render/passes.hpp"

// The scene renderer (M5.6, ADR-0022): the bridge from "a World full of entities" to "passes
// declared on a render graph". Each frame it
//
//   1. EXTRACTS — queries the World for everything renderable and copies it into flat arrays
//      (extract_scene below). After this step the World is untouched; simulation could already
//      be mutating it while the GPU work is recorded — the seam a parallel/pipelined frame
//      needs, kept even though v0 runs serially.
//   2. UPLOADS — packs the extraction into the frame/draw uniform buffers (std140 mirrors in
//      passes.hpp).
//   3. DECLARES — depth pre-pass (optional) → forward PBR → tonemap, using the pass library.
//
// It owns the GPU plumbing those steps need (uniform buffers, the 1x1 white / flat-normal fallback
// textures, the material sampler) and the three pass objects. It does NOT own the registries — meshes and
// materials belong to the scene/application; the renderer only reads them.
namespace rime::render {

// ── Extraction (GPU-free, unit-testable without a device) ────────────────────────────────────

struct ExtractedCamera {
    bool found = false;
    core::Mat4 view;                        // world → view (inverse of the camera's world matrix)
    float position[3] = {0.0f, 0.0f, 0.0f}; // eye, world space
    float fov_y = 0.87266f;                 // lens, copied from the Camera component
    float z_near = 0.1f;
    float z_far = 1000.0f;
};

struct ExtractedScene {
    std::vector<DrawItem> draws;                 // every {WorldTransform, MeshRef, MaterialRef}
    ExtractedCamera camera;                      // the FIRST active camera found
    std::vector<GpuDirectionalLight> dir_lights; // already GPU-shaped (uncapped; see render())
    std::vector<GpuPointLight> point_lights;
};

// Pull the renderable view of a World: draws, the active camera, lights. Reads WorldTransform —
// run ecs::propagate_transforms first or camera/meshes/lights sit at stale poses. Conventions
// (asserted by the M5.6 tests): a camera looks down its entity's local −z; a directional light
// shines along its entity's local −z; a point light sits at its entity's world translation.
[[nodiscard]] ExtractedScene extract_scene(ecs::World& world);

// ── The renderer ──────────────────────────────────────────────────────────────────────────────

class SceneRenderer {
public:
    SceneRenderer(rhi::Device& device,
                  const MeshRegistry& meshes,
                  const MaterialRegistry& materials);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // The frame's graph-side products. `ldr` is the displayable output (already exported);
    // `hdr` is the pre-tonemap radiance — export it yourself if you want to read it back
    // (tests do) or feed it to more passes before the tonemap you then declare manually.
    struct Output {
        RGTexture hdr;
        RGTexture ldr;
    };

    // Extract → upload → declare into `graph` (which the caller later executes). Returns invalid
    // handles (and declares nothing) when the World has no active camera. The depth pre-pass is
    // optional per frame — same pixels either way (the M5.6 proof asserts it), different cost
    // profile; measure per workload.
    Output render(RenderGraph& graph,
                  ecs::World& world,
                  rhi::Extent2D extent,
                  bool use_depth_prepass = true);

    // Constant ambient radiance (linear RGB) — the crude GI stand-in until M10. Default is a dim
    // 0.02: unlit sides stay visible, tests can still tell lit from unlit by an order of
    // magnitude.
    void set_ambient(float r, float g, float b) {
        ambient_[0] = r;
        ambient_[1] = g;
        ambient_[2] = b;
    }

private:
    void ensure_draw_capacity(std::uint32_t draw_count);

    rhi::Device& device_;
    const MeshRegistry& meshes_;
    const MaterialRegistry& materials_;

    DepthPrepass depth_prepass_;
    ForwardPbrPass forward_;
    TonemapPass tonemap_;

    rhi::BufferHandle frame_ubo_;
    rhi::BufferHandle draw_ubo_;
    std::uint32_t draw_capacity_ = 0;
    rhi::TextureHandle white_;            // 1x1 white: base-color / MR / occlusion / emissive fallback
    rhi::TextureHandle flat_normal_;      // 1x1 (128,128,255): the normal-map fallback = +Z (no bump)
    rhi::SamplerHandle material_sampler_; // trilinear + a little anisotropy, Repeat

    float ambient_[3] = {0.02f, 0.02f, 0.02f};
    bool warned_lights_ = false;
    bool warned_no_camera_ = false;

    // Per-frame arrays the pass lambdas' SceneDrawData spans point into — members (not locals)
    // because they must outlive render() and still be alive at graph.execute().
    std::vector<DrawItem> frame_draws_;
    // Five parallel per-draw texture arrays (one slot each), fallbacks already resolved — the spans
    // in SceneDrawData point at these, so they must outlive render() to graph.execute().
    std::vector<rhi::TextureHandle> frame_base_color_;
    std::vector<rhi::TextureHandle> frame_metallic_roughness_;
    std::vector<rhi::TextureHandle> frame_normal_;
    std::vector<rhi::TextureHandle> frame_occlusion_;
    std::vector<rhi::TextureHandle> frame_emissive_;
    std::vector<std::uint8_t> draw_staging_;
};

} // namespace rime::render
