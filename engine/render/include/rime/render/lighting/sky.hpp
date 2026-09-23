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
// m17.7d makes sky-view/SH a precomputed physical atmosphere and composites that same sky-view
// table into the full-resolution BACKGROUND. The only analytic presentation term left is the sharp
// solar disc, because the table cannot resolve it and the DirectionalLight owns direct light on
// geometry. A froxel volume remains the later aerial-perspective brick.
//
// Structurally this is the SSR pattern (m10.7b): read the forward pass's HDR + depth, write a
// SECOND HDR target the next stage reads. With the sky off, nothing allocates and the tonemap reads
// the raw forward HDR, so the frame is byte-identical -- ADR-0032 Section 11's rule for any pass
// that can be switched off.
struct SkyParams {
    bool enabled = false;

    // Legacy authored-gradient colours, retained so existing .rscene files round-trip. The m17.7d
    // physical background and lighting path derives its colour from the atmosphere and solar
    // source instead; use `sun_radiance`, atmosphere parameters and `intensity` to tune it.
    // They remain linear HDR radiance, not sRGB, for the legacy compute-only fallback.
    float zenith[3] = {0.13f, 0.29f, 0.66f};
    float horizon[3] = {0.62f, 0.72f, 0.86f};
    float intensity = 1.0f;
    float ground = 0.35f; // legacy fallback's below-horizon darkening

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

    // Physical atmosphere inputs are deliberately a separate block from the authored sky.  The
    // former are SI-like kilometres and inverse-kilometres used by the LUT producers; the latter
    // are art-direction controls whose values are not meaningful to a density integral.
    struct Atmosphere {
        float planet_radius_km = 6360.0f;
        float atmosphere_height_km = 100.0f;
        float rayleigh_scale_height_km = 8.0f;
        float mie_scale_height_km = 1.2f;
        float rayleigh_scattering[3] = {0.0058f, 0.0135f, 0.0331f};  // 1/km
        float mie_scattering[3] = {0.003996f, 0.003996f, 0.003996f}; // 1/km
        float mie_absorption[3] = {0.00044f, 0.00044f, 0.00044f};    // 1/km
        float ground_albedo[3] = {0.10f, 0.10f, 0.10f};
    } atmosphere{};
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

// Physical LUTs have independent dirty keys. Keeping four counters rather than one combined count
// makes it observable when a ground-only edit rebuilds transmittance (or when a scattering edit
// accidentally leaves multiple scattering stale).
struct SkyAtmosphereStats {
    std::uint32_t transmittance_filled = 0;
    std::uint32_t transmittance_reused = 0;
    std::uint32_t multiple_scattering_filled = 0;
    std::uint32_t multiple_scattering_reused = 0;
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
             const SkyInputs& inputs,
             const SkyLightBinding& lighting);

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

    [[nodiscard]] const SkyAtmosphereStats& atmosphere_stats() const noexcept {
        return atmosphere_stats_;
    }

    // The persistent LUT, for a test that wants to read back what was baked.
    [[nodiscard]] rhi::TextureHandle skyview_lut() const noexcept { return skyview_lut_; }

    [[nodiscard]] rhi::TextureHandle transmittance_lut() const noexcept {
        return transmittance_lut_;
    }

    [[nodiscard]] rhi::TextureHandle multiple_scattering_lut() const noexcept {
        return multiple_scattering_lut_;
    }

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

    // Readback is a consumer too. The physical tables have no rendering consumer until m17.7d,
    // so tests and diagnostics that copy one must report the layout it leaves behind before the
    // next graph imports it. This mirrors note_skyview_state() rather than making the owner guess.
    void note_transmittance_state(rhi::ResourceState state) noexcept {
        transmittance_state_ = state;
    }

    void note_multiple_scattering_state(rhi::ResourceState state) noexcept {
        multiple_scattering_state_ = state;
    }

private:
    // Everything the bake depends on. If none of it moved, the bake did not either -- the
    // cached-parameters test LocalShadowMap uses to decide a shadow slot can be reused.
    [[nodiscard]] static bool bake_inputs_equal(const SkyParams& a, const SkyParams& b) noexcept;
    [[nodiscard]] static bool transmittance_inputs_equal(const SkyParams& a,
                                                         const SkyParams& b) noexcept;
    [[nodiscard]] static bool multiple_scattering_inputs_equal(const SkyParams& a,
                                                               const SkyParams& b) noexcept;
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
    rhi::SamplerHandle atmosphere_sampler_;

    rhi::ShaderHandle transmittance_shader_;
    rhi::ShaderHandle multiple_scattering_shader_;
    rhi::PipelineHandle transmittance_pipeline_;
    rhi::PipelineHandle multiple_scattering_pipeline_;

    // Persistent, not transient: the whole point is that a frame which did not change the sky
    // reuses last frame's bake, and a graph-owned transient does not survive to be reused.
    rhi::TextureHandle skyview_lut_;
    rhi::TextureHandle transmittance_lut_;
    rhi::TextureHandle multiple_scattering_lut_;
    rhi::BufferHandle sh_buffer_;
    rhi::ResourceState skyview_state_ = rhi::ResourceState::Undefined;
    rhi::ResourceState transmittance_state_ = rhi::ResourceState::Undefined;
    rhi::ResourceState multiple_scattering_state_ = rhi::ResourceState::Undefined;
    rhi::ResourceState sh_state_ = rhi::ResourceState::Undefined;

    rhi::TextureHandle dummy_skyview_;
    rhi::BufferHandle dummy_sh_;

    SkyParams baked_{};        // what the current bake was made from
    SkyInputs baked_inputs_{}; // camera x/z matter only to the cached cloud slab
    bool has_bake_ = false;    // false until the first bake, so the first call always fills
    SkyParams transmittance_baked_{};
    SkyParams multiple_scattering_baked_{};
    bool has_transmittance_ = false;
    bool has_multiple_scattering_ = false;
    SkyLightingStats stats_{};
    SkyAtmosphereStats atmosphere_stats_{};
};

} // namespace rime::render
