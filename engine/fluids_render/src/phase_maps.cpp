// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids_render/phase_maps.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rime::fluids_render::detail {
namespace {
constexpr float tau = 2 * std::numbers::pi_v<float>;
constexpr unsigned side = map_side;

std::uint8_t byte(float x) {
    return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255 + .5f);
}

std::uint32_t hash(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    std::uint32_t h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA6Bu ^ seed * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    return h ^ (h >> 15);
}

// Periodic cellular ridge. A jittered g*g lattice (one site per cell, hashed from the WRAPPED cell
// id so the pattern tiles) gives F1/F2, the distances to the two nearest sites. F2-F1 is ~0 on the
// Voronoi boundary, so exp(-k(F2-F1)) is a thin vein that forks wherever three cells meet. This
// is cosmetic frost branching; it is not a crack or collision model.
float cell_ridge(float u, float v, int g, std::uint32_t seed, float k) {
    const float px = u * float(g), py = v * float(g);
    const int cx = int(std::floor(px)), cy = int(std::floor(py));
    float f1 = 9, f2 = 9;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = cx + dx, ny = cy + dy;
            const auto h =
                hash(std::uint32_t((nx % g + g) % g), std::uint32_t((ny % g + g) % g), seed);
            const float sx = float(nx) + .1f + .8f * float(h & 0xFFFF) / 65535.f;
            const float sy = float(ny) + .1f + .8f * float(h >> 16) / 65535.f;
            const float d = std::hypot(px - sx, py - sy);
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    return std::exp(-k * (f2 - f1));
}

struct Wave {
    int kx, ky;  // integer wave vector (cycles per UV tile)
    int w;       // integer temporal frequency (cycles per loop); sign = travel direction
    float amp;   // slope amplitude
    float phase; // fixed offset so waves do not all crest at the origin together
};

// Multiscale and directional: long swell, mid chop in different directions, fine capillary
// detail. Slope amplitudes fall with scale so the fine ripples read as secondary texture.
constexpr std::array<Wave, 6> waves{{{2, 1, 1, .26f, .3f},
                                     {-3, 2, -2, .20f, 1.9f},
                                     {5, -4, 2, .14f, 4.1f},
                                     {9, 7, -3, .09f, 2.6f},
                                     {14, -11, 3, .06f, 5.2f},
                                     {-19, -6, -1, .04f, .8f}}};
} // namespace

Slope water_slope(float u, float v, float cycle) {
    Slope s{0, 0};
    for (const auto& w : waves) {
        // height = sin(..); slope = d(height)/d(uv) direction, unit-normalised wave vector.
        const float len = std::hypot(float(w.kx), float(w.ky));
        const float c =
            std::cos(tau * (float(w.kx) * u + float(w.ky) * v + float(w.w) * cycle) + w.phase);
        s.x += w.amp * float(w.kx) / len * c;
        s.y += w.amp * float(w.ky) / len * c;
    }
    return s;
}

FrostField make_frost_field() {
    constexpr std::size_t n = std::size_t(side) * side;
    FrostField f;
    f.slope_x.assign(n, 0);
    f.slope_y.assign(n, 0);
    f.ridge.assign(n, 0);
    f.depth.assign(n, 0);
    std::vector<float> height(n);
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            const float u = float(x) / side, v = float(y) / side;
            // Three octaves of veins: a few long fractures, branches, then hairline frost.
            const float r1 = cell_ridge(u, v, 3, 11, 5.f);
            const float r2 = cell_ridge(u, v, 6, 23, 8.f);
            const float r3 = cell_ridge(u, v, 12, 47, 10.f);
            const auto i = std::size_t(y) * side + x;
            f.ridge[i] = std::clamp(std::max({r1, .75f * r2, .5f * r3}), 0.f, 1.f);
            // Slow polished undulation (integer frequencies => periodic): shallow, never a spike.
            const float slow = .5f + .25f * std::cos(tau * (2 * u + v) + 1.1f) +
                               .15f * std::cos(tau * (u - 3 * v) + 2.3f) +
                               .10f * std::cos(tau * (4 * u + 3 * v) + .4f);
            f.depth[i] = slow;
            height[i] = r1 + .6f * r2 + .35f * r3 + .12f * slow;
        }
    // Wrapped central difference: pixel 0 and pixel 127 difference across the seam, so the
    // normal map tiles without a visible edge.
    float peak = 1e-6f;
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            const auto at = [&](unsigned xx, unsigned yy) {
                return height[std::size_t(yy % side) * side + xx % side];
            };
            const auto i = std::size_t(y) * side + x;
            f.slope_x[i] = at(x + 1, y) - at(x + side - 1, y);
            f.slope_y[i] = at(x, y + 1) - at(x, y + side - 1);
            peak = std::max(peak, std::hypot(f.slope_x[i], f.slope_y[i]));
        }
    // Normalise so the sharpest vein flank has slope ~0.9 and the polished bulk stays near flat.
    const float gain = .9f / peak;
    for (std::size_t i = 0; i < n; ++i) {
        f.slope_x[i] *= gain;
        f.slope_y[i] *= gain;
    }
    return f;
}

void fill_normal(Pixels& out, const FrostField& frost, std::size_t level, std::size_t frame) {
    level = std::min(level, liquid_steps);
    // One slope-space blend, baked per level: wet = liquid share. Blending slopes (not unit
    // normals) is the standard way to combine height fields; normalise once at the end.
    const float wet = float(level) / float(liquid_steps);
    const float cycle = level == 0 ? 0.f : float(frame % time_frames) / float(time_frames);
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            const auto i = std::size_t(y) * side + x;
            float sx = (1 - wet) * frost.slope_x[i], sy = (1 - wet) * frost.slope_y[i];
            if (level != 0) {
                const auto w = water_slope(float(x) / side, float(y) / side, cycle);
                sx += wet * w.x;
                sy += wet * w.y;
            }
            const float inv = 1.f / std::sqrt(sx * sx + sy * sy + 1.f);
            out[i * 4] = byte(.5f + .5f * sx * inv);
            out[i * 4 + 1] = byte(.5f + .5f * sy * inv);
            out[i * 4 + 2] = byte(.5f + .5f * inv);
            out[i * 4 + 3] = 255;
        }
}

void fill_tint(Pixels& out, const FrostField& frost, std::size_t level) {
    level = std::min(level, tint_levels - 1);
    const float ice = 1.f - float(level) / float(tint_levels - 1);
    // Colours multiply the (already blue) ice base colour. Deep blue dominates; only thin veins
    // and shallow glints approach white, so there are no large uniformly white areas.
    constexpr float deep[3] = {.62f, .80f, .95f}, pale[3] = {.88f, .97f, 1.f};
    for (std::size_t i = 0; i < std::size_t(side) * side; ++i) {
        const float t = std::clamp(.55f * frost.ridge[i] * frost.ridge[i] +
                                       .30f * std::max(0.f, frost.depth[i] - .6f) / .4f,
                                   0.f,
                                   1.f);
        for (int c = 0; c < 3; ++c) {
            const float ice_c = deep[c] + (pale[c] - deep[c]) * t;
            out[i * 4 + std::size_t(c)] = byte(1.f + (ice_c - 1.f) * ice);
        }
        out[i * 4 + 3] = 255;
    }
}

DetailSelection select_detail(double seconds, double liquid) {
    const double l = std::isfinite(liquid) ? std::clamp(liquid, 0., 1.) : 1.;
    const auto level = static_cast<std::size_t>(std::round(l * double(liquid_steps)));
    std::size_t frame = 0;
    if (std::isfinite(seconds) && seconds > 0)
        frame = static_cast<std::size_t>(std::fmod(seconds, loop_seconds) * double(time_frames) /
                                         loop_seconds) %
                time_frames;
    return {level == 0 ? 0 : 1 + (level - 1) * time_frames + frame, level};
}
} // namespace rime::fluids_render::detail
