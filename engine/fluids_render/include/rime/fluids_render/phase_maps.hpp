// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Pure, deterministic CPU generation of the cosmetic phase maps (no RHI, no ECS, no clock).
// Split from PhaseDetail so periodicity, endpoint continuity and normal validity are testable
// without a GPU. Nothing here feeds back into thermal state.
namespace rime::fluids_render::detail {
inline constexpr unsigned map_side = 128;
inline constexpr unsigned map_mips = 8;
inline constexpr std::size_t time_frames = 32; // phase quantization: loop / 32 per liquid level
inline constexpr std::size_t liquid_steps = 8; // liquid fraction quantized to k/8, k = 0..8
inline constexpr std::size_t tint_levels = liquid_steps + 1;
inline constexpr double loop_seconds = 1.6;
// Level 0 (fully frozen) is one static map; levels 1..8 each hold a full time loop.
inline constexpr std::size_t normal_count = 1 + liquid_steps * time_frames;
// 4 bytes/texel over a full 128..1 mip chain: 4 * (16384+4096+1024+256+64+16+4+1) = 87,380 B.
inline constexpr std::size_t bytes_per_map = 87380;
inline constexpr std::size_t total_bytes = (normal_count + tint_levels) * bytes_per_map; // 23.2 MB

using Pixels = std::array<std::uint8_t, map_side * map_side * 4>;

struct Slope {
    float x, y;
};

// Periodic in u and v (period 1) and in `cycle` (period 1 = one loop). Every wave has integer
// spatial and temporal frequency, so UV seams and the loop point are exact, not blended.
[[nodiscard]] Slope water_slope(float u, float v, float cycle);

// Static frost, precomputed once per host: periodic multiscale cellular veins as a height field
// (differenced with wrap-around, so the texture seam is continuous), plus a blue-depth field.
struct FrostField {
    std::vector<float> slope_x, slope_y; // each map_side^2, magnitude <= 1
    std::vector<float> ridge;            // 0..1, thin vein mask
    std::vector<float> depth;            // 0..1, slow polished-ice variation
};

[[nodiscard]] FrostField make_frost_field();

// level 0 ignores `frame`: it is the frost endpoint, bit-identical for every frame.
void fill_normal(Pixels& out, const FrostField& frost, std::size_t level, std::size_t frame);
void fill_tint(Pixels& out, const FrostField& frost, std::size_t level);

struct DetailSelection {
    std::size_t normal_index; // into PhaseDetail's normal table
    std::size_t tint_index;
};

// Maps committed liquid fraction and presentation seconds to table indices. Liquid is rounded to
// k/8 and k == 0 selects the one static map, so epsilon-liquid and exactly-frozen surfaces pick
// the SAME texture: no last-fraction normal jump. Non-finite input degrades to frame 0.
[[nodiscard]] DetailSelection select_detail(double presentation_seconds, double liquid);
} // namespace rime::fluids_render::detail
