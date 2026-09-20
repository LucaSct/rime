// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

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

// What the sky hands to the passes it LIGHTS (m17.7b), as opposed to the frame it paints.
//
// Both members are always valid — `empty_binding()` stands in when there is no sky — because the
// consuming pipelines' descriptor layouts are fixed. Which state we are in is carried INSIDE the
// data (the SH buffer's tenth vec4 is a flag, and the shaders branch on it), never by a handle
// being absent. That is the same contract DdgiBinding/ShadowBinding/ClusterBinding already keep,
// and it is what lets ADR-0032 §11's "off is byte-identical" claim rest on a shader branch rather
// than on what happens to be bound.
struct SkyLightBinding {
    RGTexture skyview;          // the baked sky-view LUT (RGBA16Float), sampled by SSR and DDGI
    RGBuffer sh;                // ten vec4: nine SH coefficients, then the live flag
    rhi::SamplerHandle sampler; // linear; wraps in azimuth, clamps in elevation
};

// How the LUT/SH pair was serviced this frame. CLAUDE.md requires every skip path to carry a
// counter: a cache that silently stopped refilling looks exactly like a cache that is working, and
// a proof that cannot see what it skipped still reads as passing.
struct SkyLightingStats {
    std::uint32_t filled = 0; // frames that re-baked because the sky changed
    std::uint32_t reused = 0; // frames served from the existing bake
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

    // Bake the sky into the LUT and project it onto SH, and return what the lit passes read.
    //
    // Declares two compute passes -- `sky-view-lut` and `sky-sh` -- but ONLY when the sky actually
    // changed since the last call. The resources are persistent and imported either way, so a
    // frame under an unchanging sky pays nothing; `stats()` is how you tell the two cases apart.
    //
    // MUST be called before anything that reads the result: the forward pass, the DDGI trace and
    // the SSR resolve all consume it, so the graph needs these passes declared first for its
    // dependency edges to order them. See scene_renderer.cpp.
    [[nodiscard]] SkyLightBinding
    add_lighting(RenderGraph& graph, const SkyParams& params, const SkyInputs& inputs);

    // The no-sky placeholder: a 1x1 dummy LUT and an all-zero SH buffer, so `sky_sh_enabled()`
    // reads false and every consumer takes the constant-ambient path it took before m17.7b.
    [[nodiscard]] SkyLightBinding empty_binding(RenderGraph& graph);

    [[nodiscard]] const SkyLightingStats& stats() const noexcept { return stats_; }

    // The persistent LUT, for a test that wants to read back what was baked.
    [[nodiscard]] rhi::TextureHandle skyview_lut() const noexcept { return skyview_lut_; }

    // Tell the sky what state a CONSUMER left the LUT in — the same owner/reporter pair
    // SdfClipmap::note_level_state exists for, and for the same reason. This class owns the
    // texture and imports it every frame, but whether it ends the frame in ShaderRead or in the
    // general layout its own compute write left it in depends on something this class cannot see:
    // whether SSR or DDGI sampled it. Guessing produced exactly the symptom that comment predicts
    // — `texture_barrier 'from' disagrees with the tracked layout` from the second frame on. Not a
    // correctness bug (the backend falls back to its tracked layout and still emits a correct
    // barrier), but noise that means two systems are guessing at shared state instead of one
    // owning it.
    void note_skyview_state(rhi::ResourceState state) noexcept { skyview_state_ = state; }

private:
    // Everything the bake depends on. If none of it moved, the bake did not either -- the
    // cached-parameters test LocalShadowMap uses to decide a shadow slot can be reused.
    [[nodiscard]] static bool bake_inputs_equal(const SkyParams& a, const SkyParams& b) noexcept;
    void ensure_lighting_resources();

    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;

    // ── The lighting half (m17.7b) ────────────────────────────────────────────────────────────
    rhi::ShaderHandle skyview_shader_;
    rhi::ShaderHandle sh_shader_;
    rhi::PipelineHandle skyview_pipeline_;
    rhi::PipelineHandle sh_pipeline_;
    rhi::SamplerHandle lut_sampler_;

    // Persistent, not transient: the whole point is that a frame which did not change the sky
    // reuses last frame's bake, and a graph-owned transient does not survive to be reused.
    rhi::TextureHandle skyview_lut_;
    rhi::BufferHandle sh_buffer_;
    rhi::ResourceState skyview_state_ = rhi::ResourceState::Undefined;
    rhi::ResourceState sh_state_ = rhi::ResourceState::Undefined;

    rhi::TextureHandle dummy_skyview_;
    rhi::BufferHandle dummy_sh_;

    SkyParams baked_{};     // what the current bake was made from
    bool has_bake_ = false; // false until the first bake, so the first call always fills
    SkyLightingStats stats_{};
};

} // namespace rime::render
