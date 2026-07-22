// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

// Lighting settings (M10, ADR-0032): the single gate every M10 lighting technique hangs off. The
// governing rule (ADR-0032 §11): **off == the M5.6/ADR-0022 baseline, byte-identical** — each
// technique is a graph pass (or passes) that only runs when its toggle is on, and with everything
// off the renderer is exactly the pre-M10 forward-PBR frame. This struct is staked out now with the
// fields m10.1 needs (directional shadows); later bricks (local shadows m10.2, clustered m10.3, GI
// m10.4+) add their own fields to the same struct — cheap to reserve, expensive to retrofit once
// several passes assume its absence.
namespace rime::render {

// The most cascades a directional shadow can have. 4 is the practical ceiling for a single sun and
// bounds the shadow uniform block (GpuShadowUniforms in lighting/shadows.hpp). The shader loop and
// the cascade array texture are both sized by this.
inline constexpr std::uint32_t kMaxCascades = 4;

// The most local (spot) lights that can cast a shadow in one frame (m10.2). Each takes one layer of
// the local-shadow depth array and one slot in the local-shadow uniform block; spots past this cap
// render UNSHADOWED (honest degradation — a priority atlas that evicts by intensity × coverage is
// the m10.2 fast-follow). 8 is generous for the scenes M10 targets and keeps the array small.
inline constexpr std::uint32_t kMaxLocalShadows = 8;

// ── Clustered forward (m10.3) ─────────────────────────────────────────────────────────────────
// The froxel grid: the camera frustum diced into a 3-D grid of "frustum voxels" (froxels), one
// light list per cell. 16×9 across the screen matches a 16:9 view at ~120×120-pixel tiles at 1080p
// — small enough that a light rarely covers many tiles, large enough that 3456 cells stay cheap to
// cull and to store. 24 depth slices is the Doom-2016 number and the one the log-z partition in
// docs/math/clustered-shading.md §2 is derived against.
//
// These are compile-time constants, not settings: the grid dimensions size the list buffer, the
// dispatch, and the shader's cluster lookup, and a mismatch between any two of those is a silent
// corruption rather than an error. Tunable-at-runtime froxels would need all three to re-derive
// from one uniform — worth doing when a profile asks, not before.
inline constexpr std::uint32_t kClusterGridX = 16;
inline constexpr std::uint32_t kClusterGridY = 9;
inline constexpr std::uint32_t kClusterGridZ = 24;
inline constexpr std::uint32_t kClusterCount = kClusterGridX * kClusterGridY * kClusterGridZ;

// The most lights one froxel's list can hold. Past this the cull pass CLAMPS (drops the extra
// lights for that cell only, warns once through the overflow counter) — never writes out of
// bounds. 64 is far above what a sane scene puts in one 120-pixel-wide, one-slice-deep cell; a
// scene that hits it is telling you its lights are piled up, which the overflow stat reports.
inline constexpr std::uint32_t kMaxLightsPerCluster = 64;

struct LightingSettings {
    // Directional cascaded shadow maps (m10.1). Off by default: a caller opts in, and until it does
    // the frame is the byte-identical pre-M10 baseline (the regression bridge). The editor host and
    // the M10 samples turn it on; the M5.6/M6.4 pixel proofs leave it off and stay unchanged.
    bool shadows_enabled = false;

    // Cascade count (1..kMaxCascades). More cascades = crisper near-field shadows at more depth
    // passes; 3 is the usual sweet spot for a third-person view.
    std::uint32_t cascade_count = 3;

    // Per-cascade shadow-map resolution (square). One layer of the cascade array is this × this.
    std::uint32_t shadow_map_resolution = 2048;

    // Practical-split blend (Zhang et al.): 0 = a uniform split (even world slices, wastes texels
    // on the near field), 1 = a logarithmic split (even perspective slices, starves the far field).
    // 0.5 is the standard compromise. See docs/math/shadow-mapping.md §2.
    float cascade_split_lambda = 0.5f;

    // Shadow bias, in the units the depth compare needs (docs/math/shadow-mapping.md §4). The
    // normal offset pushes the sample point off the receiving surface along its normal (kills most
    // acne regardless of light angle); the small constant depth bias mops up the rest. Too much of
    // either detaches the shadow from its caster (peter-panning) — these defaults are tuned on
    // lavapipe.
    float shadow_normal_bias = 0.06f;
    float shadow_depth_bias = 0.0015f;

    // PCF filter radius in shadow-map texels: the half-width of the box the hardware-compare
    // samples are averaged over (a 3×3 grid stepped by this many texels). Bigger = softer edges,
    // more taps. Shared by the directional and local shadows.
    float shadow_pcf_radius = 1.0f;

    // ── Local-light (spot) shadows (m10.2) ──────────────────────────────────────────────────────
    // A second, independent gate: spot lights cast shadows through their own perspective shadow map
    // (one array layer each), sampled in the same forward pass as the sun's cascades. Off by
    // default — a scene with spot lights but this off renders them as unshadowed cones. Requires
    // shadows_enabled (spot shading rides the shadowed forward shader). The distinguishing feature
    // is the DESTRUCTIBILITY-AWARE CACHE: a spot's map is re-rendered only when its light moves or
    // a destruction event (ADR-0032 C2) invalidates the region its frustum covers — see
    // lighting/local_shadows.hpp and docs/math/shadow-mapping.md §7.
    bool local_shadows_enabled = false;

    // Per-spot shadow-map resolution (square). One layer of the local-shadow array is this × this;
    // local maps are usually smaller than cascades (a spot lights a room, not the whole view).
    std::uint32_t local_shadow_resolution = 1024;

    // ── Clustered forward shading (m10.3) ───────────────────────────────────────────────────────
    // A third independent gate. On: point lights are culled into per-froxel lists by a compute
    // pass and the forward shader loops only its own froxel's list — the ADR-0022 cap of 16 point
    // lights in a uniform block is gone, and hundreds of dynamic lights cost what the lit pixels
    // actually touch. Off (the default): the M5.6 uniform-block loop, byte-identical baseline.
    //
    // Independent of the shadow gates, but note both M10 forward paths share ONE shader — turning
    // this on with shadows off runs the shadowed shader with zero cascades and zero spots, which
    // is arithmetically the baseline (a count-0 shadow loop returns "lit").
    bool clustered_enabled = false;

    // ── The runtime SDF clipmap (m10.4b) ────────────────────────────────────────────────────────
    // A fourth independent gate. On: SceneRenderer steps its SdfClipmap every frame (recentring it
    // on the camera and recomposing whatever changed) — the field m10.5's DDGI probes will
    // sphere-trace through. Off (the default): SceneRenderer never touches it at all, so this
    // brick adds zero GPU work and the frame is exactly the pre-M10 baseline — nothing samples the
    // clipmap yet (m10.5 is the first consumer; this flag exists so that consumer has an on/off
    // switch to gate against from day one, matching every other M10 technique's discipline).
    bool sdf_clipmap_enabled = false;

    // ── DDGI global-illumination probes (m10.5a) ────────────────────────────────────────────────
    // A fifth independent gate. On: SceneRenderer steps its DdgiProbes every frame, sphere-tracing
    // a round-robin batch through the SdfClipmap and updating their octahedral irradiance/
    // visibility atlases (lighting/ddgi.hpp). Off (the default): zero GPU work, byte-identical
    // pre-M10 baseline. REQUIRES sdf_clipmap_enabled — DDGI traces the SAME field m10.4b composes,
    // the same "requires the gate underneath it" discipline local_shadows_enabled already follows
    // for shadows_enabled. Nothing SAMPLES these atlases yet (m10.5b is the first consumer,
    // matching sdf_clipmap_enabled's own "first consumer is a later brick" note).
    bool ddgi_enabled = false;

    // Probes per axis of the camera-centred lattice (clamped to >= 1). The ADR-0032 §2 / m10.4c
    // spike's measured full-grid-every-frame budget is 8x8x8=512 (~1.0-1.5x the m10.3 cluster
    // cull); a larger lattice cycles through kMaxDdgiProbesPerUpdate probes per frame instead of
    // growing per-frame cost (lighting/ddgi.hpp's round-robin).
    std::uint32_t ddgi_probe_count_x = 8;
    std::uint32_t ddgi_probe_count_y = 8;
    std::uint32_t ddgi_probe_count_z = 8;

    // World-space spacing between adjacent probes (metres). The lattice spans (count-1)*spacing
    // per axis, camera-centred and texel-snapped to this spacing exactly as the SDF clipmap's
    // levels snap to their own voxel size (lighting/ddgi.hpp) — sub-spacing camera motion must not
    // reshuffle which probe sits where.
    float ddgi_probe_spacing = 1.0f;

    // Rays per probe per update (the DDGI norm, and comfortably inside the m10.4c spike's linear-
    // cost region). Every scheduled probe casts this many spherical-Fibonacci rays, rotated by a
    // fresh per-frame random rotation so temporal accumulation converges to the true integral
    // instead of a fixed low-discrepancy bias.
    std::uint32_t ddgi_rays_per_probe = 64;

    // How far a probe ray sphere-traces before being treated as a miss (the sky term). 8 m matches
    // the SDF clipmap's level-0 extent (lighting/sdf_clipmap.hpp) — past it level 0 has nothing
    // more accurate to offer a probe anyway.
    float ddgi_max_trace_distance = 8.0f;

    // Temporal blend factor: stored = hysteresis*old + (1-hysteresis)*new each update. 0.97 is the
    // Majercik-et-al. DDGI-paper norm — slow enough that per-frame ray noise averages out, fast
    // enough that a genuine change is visible within a couple of seconds at 60 Hz. A probe's
    // FIRST-EVER update always uses 0 instead (a clean snap to that update's own estimate, never
    // blended with the atlas's initial zero) — DdgiProbes' own "primed" bookkeeping, not a setting.
    float ddgi_hysteresis = 0.97f;

    // The v1 grey-world albedo every SDF hit bounces (ADR-0032's pre-decided answer,
    // docs/math/ddgi.md §4): an SDF hit gives a position and a normal, never a material colour, so
    // real per-surface colour bleeding is a named follow-up (a surface cache), not built here. 0.6
    // is a plausible average indoor-surface albedo (matte paint / plaster).
    float ddgi_albedo = 0.6f;

    // ── Screen-space reflections (m10.7) ────────────────────────────────────────────────────────
    // A sixth independent gate. On: the shadowed forward pass ALSO writes a thin G-buffer (world
    // normal + perceptual roughness) that m10.7b's SSR compute pass marches against the depth
    // buffer. Off (the default): no G-buffer is allocated or written — byte-identical pre-M10
    // baseline. The G-buffer rides the SHADOWED forward shader, where the normal and roughness are
    // already computed, so — like clustered/local shading — turning SSR on pulls the frame onto the
    // shadowed path; with every other feature off that shader is arithmetically the baseline
    // (count-0 shadow loops return "lit"). m10.7a produces and tests the G-buffer; the SSR march
    // that consumes it (and its own march/fallback/roughness settings) is m10.7b.
    bool ssr_enabled = false;
};

} // namespace rime::render
