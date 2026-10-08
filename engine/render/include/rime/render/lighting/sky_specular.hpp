// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/render/render_graph.hpp"
#include "rime/rhi/device.hpp"

namespace rime::render {

// The sky's SPECULAR environment, split-sum style (ADR-0078 section 2).
//
// A mirror-smooth metal reflects one direction of the sky; a rough one reflects a whole cone of
// directions averaged together. Karis' "split-sum" approximation (Real Shading in Unreal Engine 4,
// SIGGRAPH 2013) makes that affordable by factoring the reflected light into two pieces that can be
// computed ahead of time:
//
//   reflected  ~=  (the sky averaged over the GGX lobe)  x  (the BRDF integrated vs a white sky)
//                   '-- the PREFILTERED chain --------'      '-- the DFG table ------------'
//
// This class owns both and builds both at RUNTIME, through the render graph, rather than loading
// baked files: the prefiltered chain because the sky is dynamic (time of day, weather, and a
// collapse that opens a room to the sun all change it), the DFG table because a table whose
// generator is the documentation beats a binary blob nobody can regenerate. Filament ships a
// runtime GPU filter and Frostbite pre-integrated dynamic probes at runtime for the same reason.
//
// The chain is described and the shaders are in sky_specular_{prefilter,dfg}.comp and
// sky_specular_eval.glsl; the numbers a reader can hold the result to are in
// tests/render/sky_specular_test.cpp.
//
// Not in this class, deliberately: occlusion of the sky by geometry. A metal inside a room still
// sees "the sky" through the ceiling here. Cone-tracing the SDF clipmap for that is the next brick
// of ADR-0078 section 2.

// Roughness levels in the chain, counting level 0. Level 0 is the unfiltered sky (a zero-width lobe
// filters to itself), so it is the sky-view LUT and costs no storage here; levels 1..N-1 live in
// the array, level k being the GGX lobe of PERCEPTUAL roughness k / (N - 1).
inline constexpr std::uint32_t kSkySpecularLevels = 7;
inline constexpr std::uint32_t kSkySpecularLayers = kSkySpecularLevels - 1;

// The array's per-layer size, in the sky-view LUT's own parameterisation. Smaller than the LUT
// (192x108) because every layer is a blur: the narrowest filtered lobe (roughness 1/6, alpha 0.028)
// is ~3-4 degrees wide, comfortably more than one 2.8-degree texel of this grid.
inline constexpr std::uint32_t kSkySpecularWidth = 128;
inline constexpr std::uint32_t kSkySpecularHeight = 72;
inline constexpr std::uint32_t kSkySpecularGroup = 8; // must match the shaders' local_size

// Importance samples per prefiltered texel / per DFG texel. Both sequences are deterministic
// (Hammersley), so these set a fixed bias, not noise.
inline constexpr std::uint32_t kSkySpecularPrefilterSamples = 1024;
inline constexpr std::uint32_t kSkySpecularDfgSamples = 2048;

// The DFG table's size: n.v along x, roughness along y, texel-centre sampled (see the shader).
inline constexpr std::uint32_t kSkySpecularDfgSize = 64;

// What the lit passes read. Both are always valid -- `empty_binding()` stands in when the feature
// is off -- because the consuming pipelines' descriptor layouts are fixed; whether the data is live
// is carried by the frame uniforms' flag (FrameUniforms::ambient[3]), never by a handle being
// absent. Same contract SkyLightBinding, DdgiBinding and ShadowBinding keep.
struct SkySpecularBinding {
    RGTexture
        prefiltered; // RGBA16F 2-D array, kSkySpecularLayers layers; alpha = acceptance counter
    RGTexture dfg;   // RGBA16F; R = scale A, G = bias B, B = acceptance counter
    rhi::SamplerHandle dfg_sampler; // linear, clamp both axes
};

// How the two bakes were serviced. CLAUDE.md requires every skip path to carry a counter: a chain
// that silently stopped refilling looks exactly like one that is working.
struct SkySpecularStats {
    std::uint32_t dfg_filled = 0;
    std::uint32_t dfg_reused = 0;
    std::uint32_t prefilter_filled = 0;
    std::uint32_t prefilter_reused = 0;
    // Frames served the placeholder: the feature was switched off, SSR or DDGI made the forward
    // pass's sky mirror unreachable, or there was no sky at all. A frame that is not counted
    // anywhere is a frame a test cannot tell from "worked and was reused".
    std::uint32_t disabled_frames = 0;
};

class SkySpecular {
public:
    explicit SkySpecular(rhi::Device& device);
    ~SkySpecular();

    SkySpecular(const SkySpecular&) = delete;
    SkySpecular& operator=(const SkySpecular&) = delete;

    // Declare the bakes this frame needs and return what the lit passes read.
    //
    // `source` is the sky as a radiance table in the sky-view parameterisation (sky_mapping.glsl),
    // sampled linearly with azimuth wrap; SkyPass hands over its sky-view LUT, a test hands over a
    // synthetic one. `source_changed` is true on a frame whose source was re-baked. The DFG table
    // is built on the first call and never again; the prefiltered chain is rebuilt when the source
    // changed, or when it has never been built, or when a disabled stretch let the source move
    // underneath it.
    //
    // The graph orders the prefilter after whatever wrote `source`, and the consumer after the
    // prefilter, from the declared reads -- so this MUST be called after the source's producer is
    // declared and before its consumers (scene_renderer.cpp).
    [[nodiscard]] SkySpecularBinding add(RenderGraph& graph, RGTexture source, bool source_changed);

    // Tell the chain its source moved while nobody was filtering it (the feature was switched off
    // for a frame that re-baked the sky), so the next enabled frame rebuilds instead of reusing a
    // chain of an earlier sky.
    void mark_stale() noexcept { prefilter_valid_ = false; }

    // The placeholders: a 1x1, 2-layer array (so it takes a 2-D-array view) and a 1x1 table.
    [[nodiscard]] SkySpecularBinding empty_binding(RenderGraph& graph);

    [[nodiscard]] const SkySpecularStats& stats() const noexcept { return stats_; }

    // The persistent resources, for a test that reads back what was baked.
    [[nodiscard]] rhi::TextureHandle prefiltered() const noexcept { return prefiltered_; }

    [[nodiscard]] rhi::TextureHandle dfg() const noexcept { return dfg_; }

    // The linear, repeat-in-azimuth sampler the source is read through -- the same configuration
    // the sky-view LUT's own sampler has, exposed so the forward pass's sampler matches.
    [[nodiscard]] rhi::SamplerHandle prefiltered_sampler() const noexcept {
        return source_sampler_;
    }

    // Tell the chain what state a CONSUMER (or a readback) left each bake in; same owner/reporter
    // pair SkyPass::note_skyview_state is. Both end a baking frame in ShaderRead, which is the
    // state the forward pass leaves them in because it samples both every frame.
    void note_prefiltered_state(rhi::ResourceState state) noexcept { prefiltered_state_ = state; }

    void note_dfg_state(rhi::ResourceState state) noexcept { dfg_state_ = state; }

private:
    void ensure_resources();

    rhi::Device& device_;
    rhi::ShaderHandle prefilter_shader_;
    rhi::ShaderHandle dfg_shader_;
    rhi::PipelineHandle prefilter_pipeline_;
    rhi::PipelineHandle dfg_pipeline_;
    rhi::SamplerHandle source_sampler_;
    rhi::SamplerHandle dfg_sampler_;

    rhi::TextureHandle prefiltered_;
    rhi::TextureHandle dfg_;
    rhi::ResourceState prefiltered_state_ = rhi::ResourceState::Undefined;
    rhi::ResourceState dfg_state_ = rhi::ResourceState::Undefined;
    bool prefilter_valid_ = false;
    bool dfg_valid_ = false;

    rhi::TextureHandle dummy_prefiltered_;
    rhi::TextureHandle dummy_dfg_;

    SkySpecularStats stats_{};
};

} // namespace rime::render
