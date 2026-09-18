// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/rhi/device.hpp"

namespace rime::render {

// The sky (m17.0): a procedural daytime sky with a sun disc and a cloud layer, composited over the
// frame wherever the depth buffer says nothing was drawn.
//
// This is an ANALYTIC sky and it is the scene's BACKGROUND, not its light source. ADR-0040 records
// where this is going -- a Hillaire-2020 precomputed-LUT atmosphere whose transmittance reddens the
// sun, whose sky-view LUT feeds ambient/DDGI, and whose froxel volume gives aerial perspective --
// and the seam here is deliberately the one that design asks for, so the physical model replaces
// the shader body without moving the pass, the parameters, or the sun coupling.
//
// Structurally this is the SSR pattern (m10.7b): read the forward pass's HDR + depth, write a
// SECOND HDR target the next stage reads. With the sky off, nothing allocates and the tonemap reads
// the raw forward HDR, so the frame is byte-identical -- ADR-0032 Section 11's rule for any pass
// that can be switched off.
struct SkyParams {
    bool enabled = false;

    // Colours are linear HDR radiance, not sRGB. `intensity` scales the whole sky at once, which is
    // the knob to reach for when the scene's exposure changes rather than re-tuning both colours.
    float zenith[3] = {0.13f, 0.29f, 0.66f};
    float horizon[3] = {0.62f, 0.72f, 0.86f};
    float intensity = 1.0f;
    float ground = 0.35f; // how far the below-horizon colour is darkened toward the ground

    // The sun disc. `angular_radius` is in radians -- the real sun subtends about 0.0047, but a
    // slightly larger disc reads better at 960x540 and in a small editor viewport.
    float angular_radius = 0.012f;
    // Sun colour/direction default to the scene's first DirectionalLight when `use_scene_sun` is
    // set, which is the coupling ADR-0040 makes physical later. With it clear, the explicit
    // direction below is used, so a test can pin the sun without a light in the world.
    bool use_scene_sun = true;
    float sun_direction[3] = {0.32f, 0.62f, -0.72f}; // TOWARD the sun
    float sun_radiance[3] = {1.0f, 0.96f, 0.86f};

    // Clouds. `coverage` is the fraction of sky they take (0 = clear, 1 = overcast); `scale` is in
    // 1/metres, so a smaller number makes larger clouds; `wind` scrolls the field and is what a
    // caller advances over time.
    bool clouds_enabled = true;
    float coverage = 0.45f;
    float density = 1.0f;
    float altitude = 1400.0f;
    float scale = 0.00035f;
    float sharpness = 0.28f; // the width of the noise band the cloud edge fades across
    float wind[2] = {0.0f, 0.0f};
};

// What the pass needs from the frame. Kept separate from SkyParams (which is authored/tuned) so the
// per-frame camera data has one obvious owner.
struct SkyInputs {
    core::Mat4 view{};
    core::Mat4 proj{};
    core::Vec3 camera_pos{};
    rhi::Extent2D extent{};
};

class SkyPass {
public:
    explicit SkyPass(rhi::Device& device);
    ~SkyPass();

    SkyPass(const SkyPass&) = delete;
    SkyPass& operator=(const SkyPass&) = delete;

    // Composites the sky from `scene_color` + `depth` into `out_hdr`. Every pixel is written (the
    // fullscreen triangle covers the target), so `out_hdr` needs no clear.
    void add(RenderGraph& graph,
             RGTexture scene_color,
             RGTexture depth,
             RGTexture out_hdr,
             const SkyParams& params,
             const SkyInputs& inputs);

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
};

} // namespace rime::render
