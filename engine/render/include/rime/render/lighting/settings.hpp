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
    // more taps.
    float shadow_pcf_radius = 1.0f;
};

} // namespace rime::render
