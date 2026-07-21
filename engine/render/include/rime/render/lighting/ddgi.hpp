// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/sdf_clipmap.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/render_graph.hpp"

// DDGI probes — the trace-and-store half (m10.5a, ADR-0032 §2). This is the second half of M10's
// thesis: m10.1-m10.2 made the SHADOW move when a wall breaks; this (plus m10.4b's clipmap) is what
// makes the BOUNCED LIGHT update too. The technique is Dynamic Diffuse Global Illumination
// (Majercik, Gallo, Falcao, Kirchhefer, Krajcevski, "Dynamic Diffuse Global Illumination with
// Ray-Traced Irradiance Fields", HPG/JCGT 2019): a lattice of PROBES, each storing an irradiance
// field over the sphere of directions, updated every frame by casting a batch of rays from the
// probe and shading what they hit. The paper traces against real geometry with hardware RT; Rime
// has no hardware RT (ADR-0032's platform floor) and instead SPHERE-TRACES through the SDF
// clipmap m10.4b already builds — the m10.4c spike measured that this is affordable at
// 8³=512 probes x 64 rays every frame.
//
// The full derivation — spherical Fibonacci sampling, the octahedral atlas encoding and its
// border (the classic bug), Chebyshev visibility, and what temporal hysteresis costs in latency —
// lives in docs/math/ddgi.md; this header cites, the doc explains.
//
// What this brick does NOT do (the honest, named gaps — docs/math/ddgi.md restates these):
//   - No per-surface albedo. An SDF hit gives a position and a normal (the field's own gradient),
//     never a material colour, so every hit bounces a single configurable GREY-WORLD albedo
//     (LightingSettings::ddgi_albedo). Real colour bleeding needs a surface cache keyed by hit
//     position/instance — a named follow-up, not built here (ADR-0032's own pre-decided answer).
//   - No CSM shadow-map sampling at a hit. Wiring CascadedShadowMap's cascade array + per-cascade
//     matrices + PCF into a COMPUTE pass built around a completely different binding shape is a
//     disproportionate coupling for this brick (the brief's own escape hatch). Instead, a hit's
//     direct-lighting term is SELF-shadowed by a second sphere-trace through the SAME clipmap
//     toward the sun — cheap (it reuses sdf_sphere_trace verbatim), architecturally free (zero new
//     coupling), and it is what makes "a probe sealed inside a closed box stays dark" true for the
//     right physical reason instead of by test-scene trickery. This is a deliberate, considered
//     substitution, not a shortcut taken by accident.
//   - No multi-bounce. A hit that is not directly sun-lit contributes exactly zero this frame —
//     single-bounce DDGI's honest limit. Probes sampling probes (indirect-of-indirect) is a named
//     follow-up.
//   - No C2 destruction hook of its own. Unlike SdfClipmap/LocalShadowMap, DdgiProbes keeps no
//     dirty-region bookkeeping: every update re-traces from whatever the clipmap CURRENTLY holds,
//     so a change becomes visible the next time that probe's round-robin turn comes up (worst
//     case: total_probes/kMaxDdgiProbesPerUpdate frames later). m10.5b's job is to shorten that by
//     lowering hysteresis (or bumping priority) inside a C2-invalidated region — see add()'s note.
namespace rime::render {

// ── The octahedral atlas shape (pre-decided sizes, docs/math/ddgi.md §3) ────────────────────────
// Irradiance is smooth (it is a cosine-weighted AVERAGE over a hemisphere) so a small tile suffices
// (the Majercik-paper norm); visibility is used for a sharper Chebyshev occlusion test later
// (m10.5b) and gets more texels for it (also the paper's own numbers: 8x8/16x16 INCLUDING the
// border). Both share the SAME 1-texel border (below) — the octahedral seam is a property of the
// mapping, not of what is stored, so both atlases need the identical fix.
inline constexpr std::uint32_t kDdgiIrradianceTileInterior = 6;
inline constexpr std::uint32_t kDdgiVisibilityTileInterior = 14;

// The border every tile carries on all four sides: hardware bilinear filtering samples up to
// half a texel past a UV, so without a border a probe's own edge would blend with WHATEVER
// texel happens to sit next to it in the atlas (a different probe's tile, or nothing) instead of
// the texel the octahedral mapping actually wraps to. One texel is exactly enough headroom for
// bilinear (docs/math/ddgi.md §3 derives the border's exact content — a per-axis "fold" of the
// UV that is bit-for-bit the same computation an interior texel does, so a single formula handles
// both without a separate copy pass).
inline constexpr std::uint32_t kDdgiTileBorder = 1;
inline constexpr std::uint32_t kDdgiIrradianceTileSize =
    kDdgiIrradianceTileInterior + 2 * kDdgiTileBorder;
inline constexpr std::uint32_t kDdgiVisibilityTileSize =
    kDdgiVisibilityTileInterior + 2 * kDdgiTileBorder;

// The round-robin ceiling (ADR-0032 §2 / the m10.4c spike): a full 8³=512-probe update every
// frame is the measured-affordable case. A grid configured larger than this cycles 1/N of itself
// per frame instead of growing per-frame cost — the "amortize" lever the spike names. Probes are
// never SKIPPED, only spread over more frames; see DdgiProbes::add()'s round-robin cursor.
inline constexpr std::uint32_t kMaxDdgiProbesPerUpdate = 512;

// ── Octahedral mapping (Cigolle et al. / Meyer et al.; docs/math/ddgi.md §3) ────────────────────
// A bijection between the unit sphere and the square [-1,1]^2 — one probe's whole irradiance field
// fits in one small 2-D tile instead of six cube faces or a lat-long image with pole singularities.
// These are the CPU mirrors of ddgi_blend_irradiance.comp / ddgi_blend_visibility.comp's identical
// GLSL functions (the cluster_bounds/cluster_cull.comp pattern: one formula, two languages, and a
// test that checks them against each other rather than trusting either alone) — used here so a test
// can ask "which texel points straight down?" without reverse-engineering the shader.
[[nodiscard]] core::Vec2 ddgi_oct_encode(core::Vec3 dir) noexcept;
[[nodiscard]] core::Vec3 ddgi_oct_decode(core::Vec2 uv) noexcept;

// Spherical Fibonacci direction i of n (Keinert et al. 2015 popularized the closed form; this
// specific golden-angle spelling matches the one already measured in the m10.4c spike,
// tests/render/sdf_clipmap_test.cpp's `fibonacci_direction` — same formula, so the ray population
// this brick traces is exactly what the spike's numbers describe). Low-discrepancy: no clustering
// at the poles the way naive uniform-random sampling would show at small N.
[[nodiscard]] core::Vec3 ddgi_fibonacci_direction(std::uint32_t i, std::uint32_t n) noexcept;

// ── The probe lattice (docs/math/ddgi.md §2) ────────────────────────────────────────────────────
// Probe (x,y,z), 0 <= x < count_x etc., decomposed from a flat 0-based index — x fastest, then y,
// then z (the same convention MeshSdfAsset::index uses). Mirrors ddgi_trace.comp's identical
// arithmetic exactly.
struct DdgiProbeCoord {
    std::uint32_t x = 0, y = 0, z = 0;
};

[[nodiscard]] DdgiProbeCoord
ddgi_probe_coord(std::uint32_t global_index, std::uint32_t count_x, std::uint32_t count_y) noexcept;

// Probe `global_index`'s world position on the lattice: `origin` is the grid's own (already
// texel-snapped) corner, exactly as a probe sits AT a lattice point (no half-cell offset — a probe
// answers "the light AT this point", not "the light in this cell").
[[nodiscard]] core::Vec3 ddgi_probe_position(std::uint32_t global_index,
                                             core::Vec3 origin,
                                             float spacing,
                                             std::uint32_t count_x,
                                             std::uint32_t count_y) noexcept;

// Where probe `global_index`'s tile sits in an atlas, in TILES (multiply by the tile's physical
// side — interior + 2*border — for texel coordinates). The 3-D grid flattens into a 2-D atlas by
// putting one whole XY-plane's worth of probes side by side and stacking Z slices as atlas ROWS —
// an arbitrary but fixed choice (any consistent packing works; this one keeps the atlas roughly
// square for the cube-ish grids M10 targets). Mirrors ddgi_blend_*.comp's identical arithmetic.
struct DdgiAtlasTile {
    std::uint32_t col = 0, row = 0;
};

[[nodiscard]] DdgiAtlasTile ddgi_atlas_tile(std::uint32_t global_index,
                                            std::uint32_t probes_per_row) noexcept;

// ── GPU mirrors ──────────────────────────────────────────────────────────────────────────────────

// One ray's result: ddgi_trace.comp writes this, both blend passes read it. Storing the ROTATED
// direction alongside the shaded radiance (rather than having the blend passes re-derive it from
// the ray index + the frame's rotation) trades a trivial amount of buffer bandwidth (32 vs 16
// bytes/ray — irrelevant at these ray counts) for a real simplification: the blend shaders need
// know nothing about spherical-Fibonacci sampling or the per-frame rotation at all, only "here are
// N (direction, radiance, distance) samples, weight and average them." Every member a vec4 so
// C++'s layout and std430 agree by construction (the GpuSpotShadow/GpuClusterUniforms discipline).
struct GpuDdgiRay {
    float direction_dist[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz = unit direction, w = hit distance
    float radiance_pad[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // rgb = shaded radiance, w unused
};

static_assert(sizeof(GpuDdgiRay) == 32, "GpuDdgiRay must be two tightly-packed vec4s");

// std140 mirror of ddgi_trace.comp's DdgiParams block.
struct GpuDdgiTraceParams {
    core::Mat4 ray_rotation; // this frame's random rotation (upper-left 3x3 meaningful)
    float grid_origin_spacing[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz snapped origin, w spacing
    std::uint32_t grid_dims_base[4] = {0, 0, 0, 0}; // xyz probe counts, w round-robin base index
    std::uint32_t probes_rays_total[4] =
        {0, 0, 0, 0}; // x probes_this_update, y rays_per_probe, z total_probes, w unused
    float sun_dir_maxdist[4] = {0.0f, -1.0f, 0.0f, 8.0f};    // xyz sun TRAVEL dir, w max trace dist
    float sun_radiance_albedo[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // rgb sun radiance, w grey albedo
    float sky_radiance_pad[4] = {0.0f, 0.0f, 0.0f, 0.0f};    // rgb sky/ambient (a miss), w unused
};

static_assert(
    sizeof(GpuDdgiTraceParams) == 160 && offsetof(GpuDdgiTraceParams, grid_origin_spacing) == 64 &&
        offsetof(GpuDdgiTraceParams, grid_dims_base) == 80 &&
        offsetof(GpuDdgiTraceParams, probes_rays_total) == 96 &&
        offsetof(GpuDdgiTraceParams, sun_dir_maxdist) == 112 &&
        offsetof(GpuDdgiTraceParams, sun_radiance_albedo) == 128 &&
        offsetof(GpuDdgiTraceParams, sky_radiance_pad) == 144,
    "GpuDdgiTraceParams no longer matches the std140 DdgiParams block in ddgi_trace.comp");

// std140 mirror of ddgi_blend_irradiance.comp / ddgi_blend_visibility.comp's shared DdgiBlendParams
// block — both shaders need only the flat bookkeeping to map a workgroup to a probe's rays and its
// atlas tile, never the grid's 3-D shape (ddgi_atlas_tile is defined on the FLAT index).
struct GpuDdgiBlendParams {
    std::uint32_t base_total_perrow_rays[4] = {
        0,
        0,
        0,
        0}; // x round-robin base, y total_probes, z atlas_probes_per_row, w rays_per_probe
};

static_assert(sizeof(GpuDdgiBlendParams) == 16, "GpuDdgiBlendParams must be one tight vec4");

// What the last add() did — read directly by the tests (ADR-0032 §11's "state what happened"
// discipline, though note DDGI's own steady state is CONTINUOUS updating, not zero passes: unlike
// SdfClipmap/LocalShadowMap, a settled scene still re-traces every scheduled probe every frame
// (temporal accumulation needs ongoing samples to stay converged against fresh noise) — DDGI's
// idle-cost story is the outer editor loop's job (m10.0-perf: skip the whole frame), not this
// pass's.
struct DdgiStats {
    std::uint32_t probes_updated = 0; // probes traced + blended this call
    std::uint32_t rays_traced = 0;    // probes_updated * rays_per_probe
    std::uint32_t newly_primed = 0;   // of probes_updated, how many were on their FIRST-EVER update
    bool grid_shifted = false;        // the lattice origin (or its dimensions) changed this call —
                                      // every probe's "primed" history was reset
};

// The lighting inputs DDGI's direct-light term needs — the CPU shape of ExtractedScene's first
// directional light plus the ambient/sky constant SceneRenderer already carries (set_ambient) —
// so this header stays free of a dependency on ExtractedScene itself (SceneRenderer builds this
// from whichever scene it already extracted, exactly as CascadeInputs is built from it for m10.1).
struct DdgiLightingInputs {
    bool has_sun = false;
    core::Vec3 sun_direction{0.0f, -1.0f, 0.0f};   // TRAVEL direction (matches GpuDirectionalLight)
    float sun_radiance[3] = {0.0f, 0.0f, 0.0f};    // color * intensity, linear
    float sky_radiance[3] = {0.02f, 0.02f, 0.02f}; // SceneRenderer's ambient constant
};

// Owns the DDGI GPU resources — the trace/blend-irradiance/blend-visibility pipelines, the two
// persistent octahedral atlases (imported, not transient: they hold TEMPORAL state across frames,
// exactly like LocalShadowMap's cached depth array), the per-frame ray-result buffer, and the
// per-probe "ever updated" bookkeeping the temporal hysteresis needs. One per SceneRenderer, gated
// by LightingSettings::ddgi_enabled (default false — off is the byte-identical M5.6/ADR-0022
// baseline, ADR-0032 §11). Requires sdf_clipmap_enabled (it traces THAT field); see settings.hpp.
class DdgiProbes {
public:
    explicit DdgiProbes(rhi::Device& device);
    ~DdgiProbes();

    DdgiProbes(const DdgiProbes&) = delete;
    DdgiProbes& operator=(const DdgiProbes&) = delete;

    // Recentre the lattice on `camera_pos` (texel-snapping to `settings.ddgi_probe_spacing`,
    // exactly as SdfClipmap's levels snap to their own voxel size — sub-spacing camera motion must
    // not reshuffle which probe sits where), pick this frame's round-robin batch, draw a fresh
    // random ray-rotation, and declare the trace + both blend compute passes into `graph`.
    //
    // m10.5b's job, noted here because this is where it hooks in: when a C2 destruction event's
    // world-bounds overlaps a probe's own footprint, that probe's NEXT update should use a lower
    // hysteresis (or jump the round-robin queue) so the bounced light updates in a couple of
    // frames instead of the ~30 the default 0.97 would take (docs/math/ddgi.md §5) — DdgiProbes
    // keeps no such bookkeeping itself (see the header's "what this does not do").
    // Takes `clipmap` by NON-const reference: DdgiProbes is a second reader of the clipmap's level
    // textures (m10.4b's SdfClipmap::add() is the first writer), and it reports back what state its
    // own sampled read left them in (SdfClipmap::note_level_state) so the clipmap's OWN next-frame
    // recompose still imports with an accurate claimed state — see note_level_state's own comment.
    void add(RenderGraph& graph,
             SdfClipmap& clipmap,
             core::Vec3 camera_pos,
             const DdgiLightingInputs& lighting,
             const LightingSettings& settings);

    [[nodiscard]] const DdgiStats& stats() const noexcept { return stats_; }

    // The lattice's current (already texel-snapped) world-space origin — what a test checks for
    // bit-identical stability under sub-spacing camera motion.
    [[nodiscard]] core::Vec3 origin() const noexcept { return origin_; }

    // Direct access to the persistent atlases (tests read them back; m10.5b samples them).
    [[nodiscard]] rhi::TextureHandle irradiance_atlas() const noexcept { return irradiance_atlas_; }

    [[nodiscard]] rhi::TextureHandle visibility_atlas() const noexcept { return visibility_atlas_; }

    [[nodiscard]] rhi::Extent2D irradiance_atlas_extent() const noexcept;
    [[nodiscard]] rhi::Extent2D visibility_atlas_extent() const noexcept;

private:
    void ensure_atlases(std::uint32_t total_probes, std::uint32_t probes_per_row);
    void ensure_ray_buffer_capacity(std::uint32_t ray_count);
    // Draw the next frame's random rotation (a uniformly random axis + angle) from the internal
    // deterministic RNG. Returns a Mat4 whose upper-left 3x3 is the rotation (translation unused).
    [[nodiscard]] core::Mat4 next_ray_rotation() noexcept;

    rhi::Device& device_;

    rhi::ShaderHandle trace_shader_;
    rhi::PipelineHandle trace_pipeline_;
    rhi::ShaderHandle blend_irradiance_shader_;
    rhi::PipelineHandle blend_irradiance_pipeline_;
    rhi::ShaderHandle blend_visibility_shader_;
    rhi::PipelineHandle blend_visibility_pipeline_;

    rhi::SamplerHandle clipmap_sampler_; // linear + ClampToEdge, for the 3 clipmap levels

    // Persistent (imported, not transient) — they hold TEMPORAL state across frames.
    rhi::TextureHandle irradiance_atlas_;                                 // RGBA16Float
    rhi::TextureHandle visibility_atlas_;                                 // RG32Float
    rhi::ResourceState irradiance_state_ = rhi::ResourceState::Undefined; // carried across frames
    rhi::ResourceState visibility_state_ = rhi::ResourceState::Undefined;
    std::uint32_t atlas_probes_per_row_ = 0; // sizes the atlases; part of "did the grid change?"
    std::uint32_t atlas_total_probes_ = 0;

    rhi::BufferHandle
        clipmap_levels_ubo_; // this frame's clipmap.gpu_levels(), re-uploaded each add()
    rhi::BufferHandle trace_params_ubo_;  // GpuDdgiTraceParams
    rhi::BufferHandle blend_params_ubo_;  // GpuDdgiBlendParams
    rhi::BufferHandle hysteresis_buffer_; // per-probe-this-frame effective hysteresis (std430)
    rhi::BufferHandle ray_buffer_;        // this frame's GpuDdgiRay[probes_this_update*rays]
    rhi::ResourceState ray_buffer_state_ = rhi::ResourceState::Undefined; // carried across frames
    std::uint32_t ray_capacity_ = 0;

    // The lattice snap state (mirrors SdfClipmap::Level exactly).
    core::Vec3 origin_{0.0f, 0.0f, 0.0f};
    bool origin_initialized_ = false;
    std::uint32_t last_count_x_ = 0, last_count_y_ = 0, last_count_z_ = 0;

    // Round-robin + per-probe temporal bookkeeping.
    std::uint32_t round_robin_cursor_ = 0;
    std::vector<bool> primed_; // has probe i ever completed an update?

    std::uint64_t rng_state_ = 0x9E3779B97F4A7C15ull; // splitmix64 state (see next_ray_rotation)

    DdgiStats stats_{};
};

} // namespace rime::render
